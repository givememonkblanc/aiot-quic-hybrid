#!/usr/bin/env python3
"""Model camera for the shared helper (relay --emu-up): one more camera of the turntable scene, run on the host.

It connects to the relay's model-camera port as a device would and publishes, on aiot/esp32 offload/jpeg, the
frames the camera's split inference sends to the helper: a frame of a recorded trace of the scene (UXGA JPEG)
decoded at 1/8 (200x150, as the camera's detector decodes it) and encoded at JPEG quality 85, one per --period,
property 0x78 = capture time (host CLOCK_MONOTONIC ms): the publication time less --pre-ms, the time a camera's frame spends
before it leaves (capture wait, decode and encode at the camera, measured 0.19 + 0.40 + 0.06 s on the real camera, 10/15). Groups are dev x 10^12 + trace sequence, so that the
relay and the helper tell its frames from the real camera's (relay EMU_GROUP_BASE). The relay delivers them to the
helper only and logs the helper's results for them (emu_result).

--peer DOWN: the model camera is a helper too (a peer of the circular compute): a second session to the relay's downstream port,
SETUP Implementation "aiot-helper/1;node=K", that subscribes to offload/jpeg and runs the camera's detector (emu_detector.py,
model-c3, under the Python with TFLite) on the frames the relay gives it, padded to --service-ms (the ESP32's helper time), and
publishes each result on helper/events with the frame's group and capture property, as the helper firmware does.
--phases busy:S,idle:S (repeated from --phase-offset): busy, the camera publishes its frames and does not lend its compute (no
subscription: the relay gives it nothing); idle, it publishes nothing and subscribes (the subscription is its capacity signal).
Without --phases: it publishes and helps all the time.

usage: emu_camera.py --up HOST:PORT --dev K --out DIR [--period 1.0] [--seconds 230] [--trace DIR] [--skip N]
                     [--peer HOST:PORT [--service-ms 310] [--phases busy:30,idle:30] [--phase-offset S]]"""
import argparse
import asyncio
import ctypes
import io
import json
import ssl
import struct
import subprocess
import time
from pathlib import Path

from PIL import Image

BASE = Path(__file__).resolve().parent
NS = (b"aiot", b"esp32")
TRACKS = [
    dict(ns=NS, name=b"offload/jpeg", priority=192, group_order=2),
    dict(ns=NS, name=b"ai/events", priority=64),
    dict(ns=NS, name=b"evidence/jpeg", priority=128),
    dict(ns=NS, name=b"status", priority=224, dynamic_groups=True),
]
CAPTURE_MS = 0x78
EMU_GROUP_BASE = 10**12
DETECTOR_PYTHON = "/home/ryzen395/miniforge3/bin/python"


def phase_of(a, t):
    """'busy' or 'idle' at t seconds from the start (no --phases: both, 'all')."""
    if not a.phases:
        return "all"
    ph = [(n, float(d)) for n, d in (x.split(":") for x in a.phases.split(","))]
    period = sum(d for _, d in ph)
    u = (t + a.phase_offset) % period
    for n, d in ph:
        if u < d:
            return n
        u -= d
    return ph[-1][0]


async def keepalive(proto, every=2.0):
    """a PING every few seconds: a camera in its idle phase sends nothing, and the relay closes a silent connection (20 s)"""
    while True:
        await asyncio.sleep(every)
        proto._quic.send_ping(int(time.monotonic() * 1000))
        proto.transmit()


def peer_class():
    from aioquic.quic.events import HandshakeCompleted, StreamDataReceived

    import moqt_wire as w
    from host_core import CorePublisher

    class Peer(CorePublisher):
        """helper session of a model peer: helper/events published by the core, offload/jpeg received here"""

        def __init__(self, *x, node=0, on_frame=None, **kw):
            super().__init__(*x, **kw)
            self.node, self.on_frame, self.bufs, self.sub_stream, self.next_rid = node, on_frame, {}, None, 0

        def quic_event_received(self, e):
            if isinstance(e, HandshakeCompleted):
                b = ctypes.create_string_buffer(256)
                self.lib.moqt_core_setup(self.core, b, 256, b"/", self.authority)   # the core's state only
                self.control = self._quic.get_next_available_stream_id(is_unidirectional=True)
                impl = f"aiot-helper/1;node={self.node}".encode()
                self._quic.send_stream_data(self.control, w.setup([(0x01, b"/"), (0x05, self.authority), (0x07, impl)]))
                self.ready.set()
                return
            if isinstance(e, StreamDataReceived):
                sid = e.stream_id
                if self.sub_stream is not None and sid in self.sub_streams_all:
                    return   # SUBSCRIBE_OK / PUBLISH_DONE of our own subscriptions
                if sid & 3 == 3:   # the relay's unidirectional streams: its control stream, or objects of offload/jpeg
                    st = self.bufs.get(sid)
                    if st is None:
                        try:
                            t, _ = w.read_vi(bytes(e.data))
                        except Exception:
                            t = None
                        st = self.bufs[sid] = bytearray() if t is not None and w.is_subgroup_type(t) else False
                    if st is not False:
                        st += e.data
                        if e.end_stream:
                            self.bufs.pop(sid, None)
                            try:
                                h = w.read_subgroup_header(bytes(st))
                                obj, _ = w.read_subgroup_object(bytes(st), h.size, h, None)
                                self.on_frame(obj)
                            except Exception as ex:
                                self.note("bad_object", error=str(ex)[:80])
                        return
            super().quic_event_received(e)

        sub_streams_all = set()

        def subscribe_frames(self):
            sid = self._quic.get_next_available_stream_id()
            self.sub_streams_all.add(sid)
            self._quic.send_stream_data(sid, w.subscribe(self.next_rid, NS, b"offload/jpeg", ()))
            self.next_rid += 2
            self.sub_stream = sid
            self.transmit()

        def unsubscribe_frames(self):
            if self.sub_stream is None:
                return
            self._quic.reset_stream(self.sub_stream, 0x1)
            self._quic.stop_stream(self.sub_stream, 0x1)
            self.sub_stream = None
            self.transmit()

    return Peer


async def peer_run(a, log, phase_now):
    """the helper half of a model peer"""
    from aioquic.asyncio import connect
    from aioquic.quic.configuration import QuicConfiguration

    from host_core import _kvp

    Peer = peer_class()
    det = subprocess.Popen([DETECTOR_PYTHON, str(BASE / "emu_detector.py")], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
    pending = asyncio.Queue(maxsize=1)

    def on_frame(obj):
        rec = dict(group=obj.group, capture_ms=obj.properties.get(CAPTURE_MS, 0), recv=time.monotonic(), data=obj.payload)
        if pending.full():   # the helper firmware keeps the newest frame only
            old = pending.get_nowait()
            log.write(json.dumps(dict(t=time.monotonic(), event="peer_drop", dev=a.dev, group=old["group"])) + "\n")
        pending.put_nowait(rec)

    host, port = a.peer.rsplit(":", 1)
    config = QuicConfiguration(is_client=True, alpn_protocols=["moqt-21"], max_datagram_frame_size=1200,
                               server_name="localhost", idle_timeout=20.0)
    config.verify_mode = ssl.CERT_NONE
    tracks = [dict(ns=NS, name=b"helper/status", priority=224), dict(ns=NS, name=b"helper/events", priority=64)]
    loop = asyncio.get_running_loop()
    async with connect(host, int(port), configuration=config,
                       create_protocol=lambda *x, **kw: Peer(*x, tracks=tracks, node=a.dev, on_frame=on_frame, **kw)) as pub:
        await pub.until(lambda: any(e.get("track") == 1 for e in pub.seen("subscribed")), a.wait_subscribe)

        async def phases():
            while True:
                want = phase_now() in ("idle", "all")
                if want and pub.sub_stream is None:
                    pub.subscribe_frames()
                    log.write(json.dumps(dict(t=time.monotonic(), event="peer_subscribe", dev=a.dev)) + "\n")
                elif not want and pub.sub_stream is not None:
                    pub.unsubscribe_frames()
                    log.write(json.dumps(dict(t=time.monotonic(), event="peer_unsubscribe", dev=a.dev)) + "\n")
                await asyncio.sleep(0.1)

        ph = asyncio.ensure_future(phases())
        ka = asyncio.ensure_future(keepalive(pub))
        try:
            while True:
                f = await pending.get()
                start = time.monotonic()
                det.stdin.write(struct.pack(">I", len(f["data"])) + f["data"])
                det.stdin.flush()
                res = json.loads(await loop.run_in_executor(None, det.stdout.readline))
                await asyncio.sleep(max(0.0, start + a.service_ms / 1000 - time.monotonic()))   # the ESP32's helper time
                end = time.monotonic()
                body = json.dumps(dict(group=f["group"], capture_ms=f["capture_ms"], dets=res["dets"], n=len(res["dets"]), ran=1,
                                       peer=a.dev)).encode()
                pub.publish(1, f["group"], body, props=_kvp([(CAPTURE_MS, f["capture_ms"])]), retain=False)
                pub.transmit()
                log.write(json.dumps(dict(t=end, event="peer_det", dev=a.dev, group=f["group"], capture_ms=f["capture_ms"],
                                          recv=f["recv"], start=start, end=end, n=len(res["dets"]))) + "\n")
                log.flush()
        finally:
            ph.cancel()
            ka.cancel()
            det.kill()


def rasters(trace, skip, count):
    """(seq, JPEG of the frame at 1/8, quality 85) for `count` frames of the trace from `skip`."""
    seqs = []
    for line in (trace / "index.csv").read_text().splitlines():
        f = line.split(",")
        if f and f[0].isdigit() and (trace / "frames" / f"{int(f[0]):07d}.jpg").exists():
            seqs.append(int(f[0]))
    seqs = sorted(seqs)
    out = []
    for seq in (seqs[skip:] + seqs[:skip])[:count]:
        im = Image.open(trace / "frames" / f"{seq:07d}.jpg")
        im.draft("RGB", (im.width // 8, im.height // 8))
        im = im.convert("RGB")
        if im.size != (200, 150):
            im = im.resize((200, 150))
        b = io.BytesIO()
        im.save(b, "JPEG", quality=85)
        out.append((seq, b.getvalue()))
    return out


async def run(a):
    from aioquic.asyncio import connect
    from aioquic.quic.configuration import QuicConfiguration

    from host_core import CorePublisher, _kvp

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    frames = rasters(Path(a.trace), a.skip, int(a.seconds / a.period) + 2)
    host, port = a.up.rsplit(":", 1)
    config = QuicConfiguration(is_client=True, alpn_protocols=["moqt-21"], max_datagram_frame_size=1200,
                               server_name="localhost", idle_timeout=20.0)
    config.verify_mode = ssl.CERT_NONE   # the relay's certificate is pinned by the firmware only (helper_bench.py)
    log = open(out / f"emu{a.dev}.jsonl", "a")
    async with connect(host, int(port), configuration=config,
                       create_protocol=lambda *x, **kw: CorePublisher(*x, tracks=TRACKS, extensions={"source_timeout": True}, **kw)) as pub:
        await pub.until(lambda: any(e.get("track") == 0 for e in pub.seen("subscribed")), a.wait_subscribe)
        log.write(json.dumps(dict(t=time.monotonic(), event="relay_subscribed", dev=a.dev)) + "\n")
        ka = asyncio.ensure_future(keepalive(pub))
        await asyncio.sleep(a.delay)
        start = time.monotonic()
        phase_now = lambda: phase_of(a, time.monotonic() - start)
        helper = asyncio.ensure_future(peer_run(a, log, phase_now)) if a.peer else None
        k = 0
        while time.monotonic() - start < a.seconds:
            await asyncio.sleep(max(0.0, start + k * a.period - time.monotonic()))
            if phase_now() == "idle":   # its scene is quiet: no frame for the helpers
                k += 1
                continue
            seq, data = frames[k % len(frames)]
            t = time.monotonic()
            ms = int(t * 1000) - a.pre_ms
            group = a.dev * EMU_GROUP_BASE + k
            d = pub.publish(0, group, data, props=_kvp([(CAPTURE_MS, ms)]), retain=False, source_age_ms=a.pre_ms)
            pub.transmit()
            log.write(json.dumps(dict(t=t, dev=a.dev, group=group, seq=seq, capture_ms=ms, bytes=len(data), delivered=len(d))) + "\n")
            log.flush()
            k += 1
        await asyncio.sleep(a.drain)
        ka.cancel()
        if helper:
            helper.cancel()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--up", required=True, help="relay --emu-up address")
    p.add_argument("--dev", type=int, required=True, help="camera number k >= 1 (groups from k x 10^12)")
    p.add_argument("--out", required=True)
    p.add_argument("--period", type=float, default=1.0, help="seconds between two frames to the helper")
    p.add_argument("--seconds", type=float, default=230.0)
    p.add_argument("--delay", type=float, default=0.0, help="seconds after the relay's subscription before the first frame")
    p.add_argument("--wait-subscribe", type=float, default=120.0)
    p.add_argument("--drain", type=float, default=3.0)
    p.add_argument("--pre-ms", type=int, default=650, help="capture to publication of a frame at a camera (ms)")
    p.add_argument("--peer", help="the relay's downstream address: the model camera is a helper too")
    p.add_argument("--service-ms", type=int, default=310, help="--peer: the helper time of a frame (ESP32 helper, 10/15)")
    p.add_argument("--phases", default="", help="busy:S,idle:S,... repeated (empty: busy and helping all the time)")
    p.add_argument("--phase-offset", type=float, default=0.0)
    p.add_argument("--trace", default=str(BASE / "results/trace-s3-long-r2/trace"))
    p.add_argument("--skip", type=int, default=0, help="first trace frame (different per camera)")
    asyncio.run(run(p.parse_args()))


if __name__ == "__main__":
    main()
