#!/usr/bin/env python3
"""Bench of the helper firmware (AIOT_MOQT_HELPER, src/main/app_moq_helper.inc).

  run      a model camera: connects to the upstream side of a relay (relay/moq_relay --offload)
           as the device, publishes the JPEG frames of a trace on track aiot/esp32 offload/jpeg
           (property 0x78 = publish time, host CLOCK_MONOTONIC ms) once per --period, and records
           the detection reports of the helper (UDP, one JSON object per inference).
  analyze  frames published / received / inferred / dropped, inference time, delay from the
           publication (the stand-in for the capture) to the arrival of the report at the host,
           and agreement of the detections with the host replica of the detector
           (replay_workers.Detector, the device pipeline validated on the camera's own results).

usage: helper_bench.py run --out DIR [--up 127.0.0.1:15468] [--report-port 15320] [--frames 120]
       helper_bench.py analyze DIR"""
import argparse
import asyncio
import hashlib
import json
import socket
import ssl
import time
from pathlib import Path

BASE = Path(__file__).resolve().parent
TRACE = BASE / "results/trace-s2-r2/trace"
NS = (b"aiot", b"esp32")
# Track layout of the camera firmware with the offload track (alias = index + 1), as the relay
# started with --offload expects it: it subscribes to ai/events, evidence/jpeg and offload/jpeg.
TRACKS = [
    dict(ns=NS, name=b"offload/jpeg", priority=192, group_order=2),
    dict(ns=NS, name=b"ai/events", priority=64),
    dict(ns=NS, name=b"evidence/jpeg", priority=128),
    dict(ns=NS, name=b"status", priority=224, dynamic_groups=True),
]
CAPTURE_MS = 0x78


def camera_tracks(trace):
    """seq -> detections of the camera itself ([MOQ_TRACK] pts in the serial log of the trace run)."""
    out = {}
    log = Path(trace).parent / "serial.log"
    if log.exists():
        for line in log.read_text(errors="replace").splitlines():
            k = line.find("[MOQ_TRACK] ")
            if k >= 0:
                try:
                    e = json.loads(line[k + 12:])
                except ValueError:
                    continue
                out[e["seq"]] = [tuple(p) for p in e["pts"]]
    return out


def trace_frames(trace):
    rows = []
    for line in (trace / "index.csv").read_text().splitlines():
        f = line.split(",")
        if f and f[0].isdigit() and (trace / "frames" / f"{int(f[0]):07d}.jpg").exists():
            rows.append(int(f[0]))
    return sorted(rows)


class Reports(asyncio.DatagramProtocol):
    def __init__(self, path):
        self.f = open(path, "a")

    def datagram_received(self, data, addr):
        t = time.monotonic()
        try:
            obj = json.loads(data)
        except ValueError:
            obj = dict(raw=data.decode(errors="replace"))
        self.f.write(json.dumps(dict(t=t, src=addr[0], report=obj)) + "\n")
        self.f.flush()


async def run(a):
    from aioquic.asyncio import connect
    from aioquic.quic.configuration import QuicConfiguration

    from host_core import CorePublisher, _kvp

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    loop = asyncio.get_running_loop()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", a.report_port))
    transport, _ = await loop.create_datagram_endpoint(lambda: Reports(out / "reports.jsonl"), sock=sock)
    seqs = trace_frames(Path(a.trace))
    if a.camera_frames:
        # only the frames the camera inferred itself, so that the helper can be compared with it
        cam = camera_tracks(a.trace)
        seqs = [s for s in seqs if s in cam]
    seqs = seqs[a.skip:a.skip + a.frames]
    host, port = a.up.rsplit(":", 1)
    config = QuicConfiguration(is_client=True, alpn_protocols=["moqt-21"], max_datagram_frame_size=1200,
                               server_name="localhost", idle_timeout=20.0)
    # the relay certificate is the one the firmware pins (private/cert.pem); it has expired, and the
    # firmware checks only the pin, so the bench publisher does not verify it either
    config.verify_mode = ssl.CERT_NONE
    log = open(out / "published.jsonl", "a")
    async with connect(host, int(port), configuration=config,
                       create_protocol=lambda *x, **kw: CorePublisher(*x, tracks=TRACKS, **kw)) as pub:
        await pub.until(lambda: any(e.get("track") == 0 for e in pub.seen("subscribed")), 10)
        print(json.dumps(dict(t=time.monotonic(), event="relay_subscribed")), flush=True)
        # the helper may connect to the relay later than this publisher: wait for it
        await asyncio.sleep(a.wait)
        start = time.monotonic()
        for k, seq in enumerate(seqs):
            await asyncio.sleep(max(0.0, start + k * a.period - time.monotonic()))
            data = (Path(a.trace) / "frames" / f"{seq:07d}.jpg").read_bytes()
            t = time.monotonic()
            ms = int(t * 1000)
            d = pub.publish(0, seq, data, props=_kvp([(CAPTURE_MS, ms)]), retain=False)
            pub.transmit()
            rec = dict(t=t, group=seq, capture_ms=ms, bytes=len(data), delivered=len(d),
                       sha256=hashlib.sha256(data).hexdigest())
            log.write(json.dumps(rec) + "\n")
            log.flush()
        await asyncio.sleep(a.drain)
    transport.close()


async def standin(a):
    """Stand-in for the helper board on the relay's downstream port: SETUP with Implementation
    aiot-helper/1, and one JSON Object per --period on helper/events once the relay subscribed."""
    import ctypes

    from aioquic.asyncio import connect
    from aioquic.quic.configuration import QuicConfiguration
    from aioquic.quic.events import HandshakeCompleted

    import moqt_wire as w
    from host_core import CorePublisher, _kvp

    class Standin(CorePublisher):
        def quic_event_received(self, e):
            if isinstance(e, HandshakeCompleted):
                b = ctypes.create_string_buffer(256)
                self.lib.moqt_core_setup(self.core, b, 256, b"/", self.authority)  # the core's state only
                self.control = self._quic.get_next_available_stream_id(is_unidirectional=True)
                self._quic.send_stream_data(self.control, w.setup([(0x01, b"/"), (0x05, self.authority), (0x07, b"aiot-helper/1")]))
                self.ready.set()
                return
            super().quic_event_received(e)

    tracks = [dict(ns=NS, name=b"helper/status", priority=224), dict(ns=NS, name=b"helper/events", priority=64)]
    host, port = a.down.rsplit(":", 1)
    config = QuicConfiguration(is_client=True, alpn_protocols=["moqt-21"], max_datagram_frame_size=1200,
                               server_name="localhost", idle_timeout=20.0)
    config.verify_mode = ssl.CERT_NONE
    async with connect(host, int(port), configuration=config,
                       create_protocol=lambda *x, **kw: Standin(*x, tracks=tracks, **kw)) as pub:
        await pub.until(lambda: any(e.get("track") == 1 for e in pub.seen("subscribed")), 10)
        print(json.dumps(dict(t=time.monotonic(), event="relay_subscribed_helper_events")), flush=True)
        for k in range(a.count):
            ms = int(time.monotonic() * 1000)
            group = 1000 + k
            body = json.dumps(dict(group=group, capture_ms=ms, dets=[[1, 2]], n=1, standin=True)).encode()
            pub.publish(1, group, body, props=_kvp([(CAPTURE_MS, ms)]), retain=False)
            pub.transmit()
            await asyncio.sleep(a.period)


def analyze(a):
    import numpy as np
    from replay_workers import Detector

    out = Path(a.dir)
    pub = [json.loads(x) for x in (out / "published.jsonl").read_text().splitlines()]
    reps = [json.loads(x) for x in (out / "reports.jsonl").read_text().splitlines()] if (out / "reports.jsonl").exists() else []
    by_group = {p["group"]: p for p in pub}
    rep = [r for r in reps if "group" in r["report"] and r["report"]["group"] in by_group]
    det = Detector()
    cam = camera_tracks(a.trace)
    cam_n = cam_exact = cam_count = 0
    exact = near = count_eq = 0
    delays, infer, queue, recv_gap = [], [], [], []
    mismatches = []
    for r in rep:
        x = r["report"]
        p = by_group[x["group"]]
        delays.append(r["t"] - p["t"])
        infer.append((x["end_us"] - x["start_us"]) / 1e6)
        queue.append((x["start_us"] - x["recv_us"]) / 1e6)
        host = det(Path(a.trace) / "frames" / f"{x['group']:07d}.jpg")
        dev = [tuple(d) for d in x["dets"]]
        if x["group"] in cam:
            cam_n += 1
            cam_exact += sorted(cam[x["group"]]) == sorted(dev)
            cam_count += len(cam[x["group"]]) == len(dev)
        count_eq += len(host) == len(dev)
        if sorted(host) == sorted(dev):
            exact += 1
        else:
            mismatches.append(dict(group=x["group"], host=host, device=dev))
        if len(host) == len(dev):
            rest = list(dev)
            ok = True
            for hx, hy in host:
                j = next((k for k, (u, v) in enumerate(rest) if abs(u - hx) <= 7 and abs(v - hy) <= 7), None)
                if j is None:
                    ok = False
                    break
                rest.pop(j)
            near += ok

    def st(v):
        v = np.array(v)
        return dict(n=len(v), mean=float(v.mean()), p50=float(np.median(v)), p95=float(np.percentile(v, 95)),
                    max=float(v.max())) if len(v) else None

    # the same results over MoQ: helper -> relay (helper/events) -> subscriber (helper_events_sub.py)
    ev = [json.loads(x) for x in (out / "events.jsonl").read_text().splitlines()] if (out / "events.jsonl").exists() else []
    ev = [e for e in ev if e.get("group") in by_group]
    moq_delay = [e["t"] - by_group[e["group"]]["t"] for e in ev]
    udp_t = {r["report"]["group"]: r["t"] for r in rep}
    moq_minus_udp = [e["t"] - udp_t[e["group"]] for e in ev if e["group"] in udp_t]
    same_payload = sum(1 for e in ev for r in rep if r["report"]["group"] == e["group"] and r["report"] == e["report"])
    span = pub[-1]["t"] - pub[0]["t"] if len(pub) > 1 else 0
    last = rep[-1]["report"] if rep else {}
    summary = dict(
        published=len(pub), publish_rate_hz=(len(pub) - 1) / span if span else None, frame_bytes=st([p["bytes"] for p in pub]),
        reports=len(rep), inferred=sum(1 for r in rep if r["report"].get("ran")),
        dropped_by_helper=last.get("dropped"), not_reported=len(pub) - len(rep),
        inference_rate_hz=(len(rep) - 1) / (rep[-1]["t"] - rep[0]["t"]) if len(rep) > 1 else None,
        inference_s=st(infer), wait_before_inference_s=st(queue), delay_publish_to_report_s=st(delays),
        detections_exact=exact, detections_exact_rate=exact / len(rep) if rep else None,
        detections_within_one_cell=near, count_equal=count_eq,
        moq_events=len(ev), moq_events_same_as_udp=same_payload, delay_publish_to_moq_event_s=st(moq_delay),
        moq_event_after_udp_s=st(moq_minus_udp),
        camera_compared=cam_n, camera_exact=cam_exact, camera_count_equal=cam_count, mismatches=mismatches[:20])
    (out / "summary.json").write_text(json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--out", required=True)
    r.add_argument("--up", default="127.0.0.1:15468")
    r.add_argument("--report-port", type=int, default=15320)
    r.add_argument("--trace", default=str(TRACE))
    r.add_argument("--frames", type=int, default=120)
    r.add_argument("--skip", type=int, default=0)
    r.add_argument("--period", type=float, default=1.0)
    r.add_argument("--camera-frames", action="store_true", help="publish only the frames the camera inferred in the trace run")
    r.add_argument("--wait", type=float, default=5.0, help="seconds between the relay's SUBSCRIBE and the first frame")
    r.add_argument("--drain", type=float, default=5.0)
    n = sub.add_parser("analyze")
    n.add_argument("dir")
    n.add_argument("--trace", default=str(TRACE))
    t = sub.add_parser("standin")
    t.add_argument("--down", default="127.0.0.1:15600")
    t.add_argument("--count", type=int, default=10)
    t.add_argument("--period", type=float, default=1.0)
    a = ap.parse_args()
    if a.cmd == "run":
        asyncio.run(run(a))
    elif a.cmd == "standin":
        asyncio.run(standin(a))
    else:
        analyze(a)


if __name__ == "__main__":
    main()
