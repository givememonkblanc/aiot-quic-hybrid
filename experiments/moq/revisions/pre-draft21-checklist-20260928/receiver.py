"""Draft21 direct subscriber for the bounded JPEG research profile; not a relay."""

import argparse, asyncio, hashlib, io, json, time
from pathlib import Path
from aioquic.asyncio import QuicConnectionProtocol, serve
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import (
    HandshakeCompleted,
    StreamDataReceived,
    ConnectionTerminated,
    StreamReset,
)
from PIL import Image

ALPN = "moqt-21"
MAX_JPEG = 256 * 1024


class NeedMore(Exception):
    pass


def vi(v):
    if not 0 <= v < 2**64:
        raise ValueError("integer range")
    n = next((n for n in range(1, 9) if v < 2 ** (7 * n)), 9)
    if n == 9:
        return b"\xff" + v.to_bytes(8, "big")
    return bytes([((0xFF << (9 - n)) & 255) | (v >> (8 * (n - 1)))]) + v.to_bytes(n, "big")[1:]


def read_vi(b, p=0):
    if p >= len(b):
        raise NeedMore
    n = 1
    mask = 128
    while mask and b[p] & mask:
        n += 1
        mask >>= 1
    if len(b) - p < n:
        raise NeedMore
    v = 0 if n == 9 else b[p] & ((1 << (8 - n)) - 1)
    for x in b[p + 1 : p + n]:
        v = (v << 8) | x
    return v, p + n


def msg(t, b):
    return vi(t) + len(b).to_bytes(2, "big") + b


def field(b):
    return vi(len(b)) + b


def read_message(b):
    t, p = read_vi(b)
    if len(b) - p < 2:
        raise NeedMore
    n = int.from_bytes(b[p : p + 2], "big")
    p += 2
    if n > 2048:
        raise ValueError("control limit")
    if len(b) - p < n:
        raise NeedMore
    return t, bytes(b[p : p + n]), p + n


def setup_fields(b):
    p = 0
    prev = 0
    out = {}
    while p < len(b):
        delta, p = read_vi(b, p)
        key = prev + delta
        prev = key
        if key >= 2**64:
            raise ValueError("option overflow")
        value, p = read_vi(b, p)
        if key & 1:
            if value > 65535 or p + value > len(b):
                raise ValueError("option length")
            value = bytes(b[p : p + value])
            p += len(value)
        if key in (1, 5) and key in out:
            raise ValueError("duplicate option")
        out[key] = value
    return out


class Receiver(QuicConnectionProtocol):
    def __init__(self, *a, out, authority, **kw):
        super().__init__(*a, **kw)
        self.out = out
        self.authority = authority
        self.buffers = {}
        self.fins = set()
        self.setup = False
        self.subscribed = False
        self.control = None
        self.request = None
        self.groups = set()
        self.failed = False
        self.count = 0
        self.expected = None

    def log(self, event, **kw):
        record = dict(t=time.monotonic(), event=event, **kw)
        with (self.out / "events.jsonl").open("a") as f:
            f.write(json.dumps(record) + "\n")
        print(json.dumps(record), flush=True)

    def error(self, reason):
        if not self.failed:
            self.log("error", reason=reason)
        self.failed = True
        self.close(error_code=3, reason_phrase=reason)

    def quic_event_received(self, event):
        if isinstance(event, HandshakeCompleted):
            if event.alpn_protocol != ALPN or not self._quic._remote_max_datagram_frame_size:
                self.error("ALPN or DATAGRAM negotiation")
                return
            self.log("handshake", alpn=event.alpn_protocol)
            sid = self._quic.get_next_available_stream_id(is_unidirectional=True)
            self._quic.send_stream_data(sid, msg(0x2F00, b""))
        elif isinstance(event, StreamReset):
            self.on_stream_reset(event.stream_id, event.error_code)
        elif isinstance(event, ConnectionTerminated):
            self.log("closed", code=event.error_code, reason=event.reason_phrase)
        elif isinstance(event, StreamDataReceived):
            sid = event.stream_id
            if sid not in self.buffers and len(self.buffers) >= 8:
                self.error("stream limit")
                return
            buf = self.buffers.setdefault(sid, bytearray())
            buf.extend(event.data)
            if len(buf) > MAX_JPEG + 64:
                self.error("object size limit")
                return
            if event.end_stream:
                self.fins.add(sid)
            try:
                self.consume(sid)
            except NeedMore:
                if sid in self.fins:
                    self.error("truncated stream")
            except (ValueError, AssertionError, OSError) as e:
                self.error(str(e))

    def on_stream_reset(self, sid, code):
        self.buffers.pop(sid, None)
        self.fins.discard(sid)
        self.log("reset", stream=sid, code=code)

    def consume(self, sid):
        b = self.buffers[sid]
        if sid == self.request:
            t, body, n = read_message(b)
            if self.subscribed:
                if t != 0x0B or self.expected is not None:
                    raise ValueError("unexpected subscription response")
                if sid not in self.fins:
                    return
                status, p = read_vi(body)
                count, p = read_vi(body, p)
                reason, p = read_vi(body, p)
                if status != 2 or count > 10000 or p + reason != len(body) or sid not in self.fins:
                    raise ValueError("invalid publish done")
                del b[:n]
                self.expected = count
                self.log("publish_done", streams=count, status=status)
                self.maybe_finish()
                return
            if t != 4 or body != b"\x01\x00":
                raise ValueError("unexpected subscription response")
            del b[:n]
            self.subscribed = True
            self.log("subscribed", track="aiot/esp32/preview/jpeg", alias=1)
            if b:
                self.consume(sid)
            for other in list(self.fins):
                if other != sid and other in self.buffers:
                    self.consume(other)
            return
        t, _ = read_vi(b)
        if t == 0x2F00 or sid == self.control:
            if self.setup or self.control is not None or sid % 4 != 2:
                raise ValueError("duplicate/invalid control stream")
            t, body, n = read_message(b)
            options = setup_fields(body)
            if options.get(1) != b"/" or options.get(5) != self.authority.encode():
                raise ValueError("authority/path mismatch")
            if sid in self.fins:
                raise ValueError("control FIN")
            if len(b) != n:
                raise ValueError("unsupported extra control message")
            self.setup = True
            self.control = sid
            b.clear()
            self.log("setup")
            self.request = self._quic.get_next_available_stream_id()
            body = vi(1) + vi(2) + field(b"aiot") + field(b"esp32") + field(b"preview/jpeg") + vi(0)
            self._quic.send_stream_data(self.request, msg(3, body))
            return
        if sid % 4 != 2 or t != 0x78:
            raise ValueError("unsupported subgroup profile")
        if sid not in self.fins or not self.subscribed:
            return
        _, p = read_vi(b)
        alias, p = read_vi(b, p)
        group, p = read_vi(b, p)
        obj, p = read_vi(b, p)
        size, p = read_vi(b, p)
        if alias != 1 or obj != 0 or not 0 < size <= MAX_JPEG or len(b) - p != size:
            raise ValueError("object identity/length")
        if group in self.groups:
            raise ValueError("duplicate group")
        data = bytes(b[p:])
        im = Image.open(io.BytesIO(data))
        im.load()
        if im.format != "JPEG":
            raise ValueError("not JPEG")
        self.groups.add(group)
        self.count += 1
        (self.out / f"frame-{group:06d}.jpg").write_bytes(data)
        self.log(
            "object",
            group=group,
            object=0,
            bytes=size,
            sha256=hashlib.sha256(data).hexdigest(),
            width=im.width,
            height=im.height,
        )
        del self.buffers[sid]
        self.fins.discard(sid)
        self.maybe_finish()

    def maybe_finish(self):
        if self.expected is not None and self.count == self.expected:
            self._quic.send_stream_data(self.request, b"", end_stream=True)
            self.transmit()
            self.log("finished", objects=self.count)
            asyncio.get_running_loop().call_later(0.3, lambda: self.close(error_code=0))


class DualReceiver(Receiver):
    """Two-track fixed profile. Counts/FIN are independent for each subscription."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.tracks = {}
        self.requests = {}
        self.all_finished = False
        self.stream_alias = {}
        self.stream_group = {}
        self.unknown_resets = set()

    def identify_stream(self, sid):
        if (
            sid in self.stream_alias
            or sid in self.requests
            or sid == self.control
            or sid not in self.buffers
        ):
            return
        try:
            t, p = read_vi(self.buffers[sid])
            alias, p = read_vi(self.buffers[sid], p)
            group, p = read_vi(self.buffers[sid], p)
        except NeedMore:
            return
        if t == 0x78 and alias in self.tracks:
            self.stream_alias[sid] = alias
            self.stream_group[sid] = group

    def on_stream_reset(self, sid, code):
        self.identify_stream(sid)
        alias = self.stream_alias.pop(sid, None)
        group = self.stream_group.pop(sid, None)
        self.buffers.pop(sid, None)
        self.fins.discard(sid)
        if alias is None:
            self.unknown_resets.add(sid)
        else:
            self.tracks[alias]["resets"].add(sid)
        self.log("reset", stream=sid, code=code, alias=alias, group=group)
        self.maybe_finish()

    def consume(self, sid):
        b = self.buffers[sid]
        if sid in self.requests:
            alias = self.requests[sid]
            track = self.tracks[alias]
            t, body, n = read_message(b)
            if not track["subscribed"]:
                if t != 4 or body != vi(alias) + vi(0):
                    raise ValueError("invalid track reply")
                track["subscribed"] = True
                del b[:n]
                self.log("subscribed", alias=alias, track=track["name"])
                if b:
                    self.consume(sid)
                for other in list(self.fins):
                    if other not in self.requests and other in self.buffers:
                        self.consume(other)
            else:
                if t != 0x0B or track["expected"] is not None:
                    raise ValueError("unexpected track message")
                if sid not in self.fins:
                    return
                status, p = read_vi(body)
                count, p = read_vi(body, p)
                reason, p = read_vi(body, p)
                if (
                    status != 2
                    or count > 10000
                    or reason > 1024
                    or p + reason != len(body)
                    or n != len(b)
                ):
                    raise ValueError("invalid track done")
                if count < len(track["groups"]) + len(track["resets"]):
                    raise ValueError("inconsistent stream count")
                reason_text = bytes(body[p : p + reason]).decode("utf-8")
                track["expected"] = count
                b.clear()
                self.log("publish_done", alias=alias, streams=count, reason=reason_text)
                self.maybe_finish()
            return
        t, _ = read_vi(b)
        if t == 0x2F00 or sid == self.control:
            super().consume(sid)
            if self.setup and not self.tracks:
                self.requests[self.request] = 1
                self.tracks[1] = dict(
                    name="preview/jpeg",
                    subscribed=False,
                    expected=None,
                    groups=set(),
                    resets=set(),
                    request=self.request,
                    finished=False,
                )
                second = self._quic.get_next_available_stream_id()
                self.requests[second] = 2
                self.tracks[2] = dict(
                    name="ai/events",
                    subscribed=False,
                    expected=None,
                    groups=set(),
                    resets=set(),
                    request=second,
                    finished=False,
                )
                self._quic.send_stream_data(
                    second,
                    msg(
                        3,
                        vi(3)
                        + vi(2)
                        + field(b"aiot")
                        + field(b"esp32")
                        + field(b"ai/events")
                        + vi(0),
                    ),
                )
            return
        if sid % 4 != 2 or t != 0x78:
            raise ValueError("unsupported subgroup")
        self.identify_stream(sid)
        if sid not in self.fins:
            return
        _, p = read_vi(b)
        alias, p = read_vi(b, p)
        group, p = read_vi(b, p)
        obj, p = read_vi(b, p)
        size, p = read_vi(b, p)
        if alias not in self.tracks or obj != 0 or not 0 < size <= MAX_JPEG or len(b) - p != size:
            raise ValueError("object identity/length")
        track = self.tracks[alias]
        if not track["subscribed"]:
            return
        if group in track["groups"]:
            raise ValueError("duplicate group")
        if (
            track["expected"] is not None
            and len(track["groups"]) + len(track["resets"]) >= track["expected"]
        ):
            raise ValueError("extra object")
        data = bytes(b[p:])
        details = {}
        if alias == 1:
            im = Image.open(io.BytesIO(data))
            im.load()
            if im.format != "JPEG":
                raise ValueError("not JPEG")
            (self.out / f"frame-{group:06d}.jpg").write_bytes(data)
            details = dict(width=im.width, height=im.height)
        else:
            if size > 4096:
                raise ValueError("AI object too large")
            result = json.loads(data)
            # Native experiments may carry either the deployed TFLM/FOMO backend (2)
            # or the opt-in ESP-DL/ESPDet backend (3).  The profile requires a real
            # model result and matching identities; it does not prescribe one model.
            if (
                result.get("schema") != 1
                or result.get("seq") != group
                or result.get("backend") not in (2, 3)
            ):
                raise ValueError("AI identity/backend")
            wire = bytes.fromhex(result["result_hex"])
            if len(wire) < 20 or wire[:2] != b"\xd1\x02" or wire[2] != result["backend"]:
                raise ValueError("AI result encoding")
            details = dict(result=result)
        track["groups"].add(group)
        self.log(
            "object",
            alias=alias,
            group=group,
            object=0,
            bytes=size,
            sha256=hashlib.sha256(data).hexdigest(),
            **details,
        )
        del self.buffers[sid]
        self.fins.discard(sid)
        self.stream_alias.pop(sid, None)
        self.stream_group.pop(sid, None)
        self.maybe_finish()

    def maybe_finish(self):
        for alias, t in self.tracks.items():
            if (
                not t["finished"]
                and t["expected"] is not None
                and len(t["groups"]) + len(t["resets"]) == t["expected"]
            ):
                t["finished"] = True
                self._quic.send_stream_data(t["request"], b"", end_stream=True)
                self.transmit()
                self.log(
                    "track_finished", alias=alias, objects=len(t["groups"]), resets=len(t["resets"])
                )
        if (
            len(self.tracks) == 2
            and all(t["finished"] for t in self.tracks.values())
            and not self.all_finished
        ):
            self.all_finished = True
            self.log("finished", objects=sum(len(t["groups"]) for t in self.tracks.values()))
            asyncio.get_running_loop().call_later(0.3, lambda: self.close(error_code=0))


async def main(args):
    args.out.mkdir(parents=True, exist_ok=True, mode=0o700)
    config = QuicConfiguration(
        is_client=False,
        alpn_protocols=[ALPN],
        max_datagram_frame_size=1200,
        max_data=2**20,
        max_stream_data=2**19,
        idle_timeout=20,
    )
    config.load_cert_chain(args.cert, args.key)
    server = await serve(
        args.host,
        args.port,
        configuration=config,
        create_protocol=lambda *a, **kw: (DualReceiver if args.dual else Receiver)(
            *a, out=args.out, authority=args.authority, **kw
        ),
    )
    try:
        await asyncio.sleep(args.seconds)
    finally:
        server.close()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--dual", action="store_true")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=14568)
    p.add_argument("--authority", default="localhost:14568")
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=120)
    asyncio.run(main(p.parse_args()))
