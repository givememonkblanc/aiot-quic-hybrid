"""Draft-21 experiment receiver: results are pushed, evidence is pushed or fetched.

Writes events.jsonl with the event names of receiver.py (handshake, subscribed, object,
publish_done, track_finished, finished, reset, error, closed) so that the existing
analyzers keep working, and adds fetch_request, fetch_object and fetch_failed.
Track aliases follow the firmware: 1 preview/jpeg, 2 ai/events, 3 evidence/jpeg, 4 status.
"""

import argparse
import asyncio
import hashlib
import io
import json
import time
from pathlib import Path

from aioquic.asyncio import serve
from aioquic.quic.configuration import QuicConfiguration
from PIL import Image

import moqt_wire as w
from subscriber import ALPN, RequestFailed, Subscriber

NS = (b"aiot", b"esp32")
CAPTURE_MS, DETECTIONS, EVIDENCE_REQUIRED, REPRESENTATION = 0x78, 0x7A, 0x7C, 0x7E
ALIASES = {b"preview/jpeg": 1, b"ai/events": 2, b"evidence/jpeg": 3, b"status": 4}


def describe(alias, payload):
    """Validates a payload and returns the fields logged with it."""
    if alias in (1, 3):
        im = Image.open(io.BytesIO(payload))
        im.load()
        if im.format != "JPEG":
            raise ValueError("not JPEG")
        return dict(width=im.width, height=im.height)
    if alias == 2:
        result = json.loads(payload)
        if result.get("schema") != 1 or result.get("backend") not in (2, 3):
            raise ValueError("AI identity/backend")
        return dict(result=result)
    return dict(status=json.loads(payload))


class ExperimentReceiver(Subscriber):
    def __init__(
        self, *a, out, authority, evidence, image_track, status, fetch_retry_s, fetch_window_s, source_timeout_ms=0, **kw
    ):
        super().__init__(*a, log=self.write, **kw)
        self.source_timeout_ms = source_timeout_ms
        self.out = out
        self.authority = authority
        self.evidence = evidence
        self.image_track = image_track
        self.status = status
        self.fetch_retry_s = fetch_retry_s
        self.fetch_window_s = fetch_window_s
        self.tasks = []
        self.started = False
        self.all_finished = False

    def write(self, record):
        if record["event"] in ("fetch_object", "object"):
            return  # logged with the payload digest by the overrides below
        with (self.out / "events.jsonl").open("a") as f:
            f.write(json.dumps(record, default=str) + "\n")
        print(json.dumps(record, default=str), flush=True)

    def record(self, event, **kw):
        record = dict(t=time.monotonic(), event=event, **kw)
        self.events.append(record)
        with (self.out / "events.jsonl").open("a") as f:
            f.write(json.dumps(record, default=str) + "\n")
        print(json.dumps(record, default=str), flush=True)
        self.changed.set()

    def dispatch(self, e):
        super().dispatch(e)
        if self.setup_done.is_set() and not self.started:
            self.started = True
            if self.peer_setup.get(5) != self.authority.encode():
                self.fail("authority mismatch", 0x19)
                return
            self.tasks.append(asyncio.ensure_future(self.run()))

    def object_record(self, event, alias, obj, **extra):
        details = describe(alias, obj.payload)
        if alias in (1, 3):
            name = f"frame-{obj.group:06d}.jpg" if alias == 1 else f"evidence-{obj.group:06d}.jpg"
            (self.out / name).write_bytes(obj.payload)
        if alias == 2 and details["result"].get("seq") != obj.group:
            raise ValueError("result identity does not match its Group ID")
        self.record(
            event,
            alias=alias,
            group=obj.group,
            object=obj.object,
            subgroup=obj.subgroup,
            priority=obj.priority,
            datagram=obj.datagram,
            properties={str(k): v for k, v in obj.properties.items() if isinstance(v, int)},
            bytes=len(obj.payload),
            sha256=hashlib.sha256(obj.payload).hexdigest(),
            **details,
            **extra,
        )

    def deliver(self, obj, sid):
        before = sum(len(s.objects) for subs in self.by_alias.values() for s in subs)
        super().deliver(obj, sid)
        after = sum(len(s.objects) for subs in self.by_alias.values() for s in subs)
        if after == before or obj.status != w.STATUS_NORMAL:
            return
        self.object_record("object", obj.alias, obj)
        wants = obj.alias == 2 and self.evidence == "fetch" and obj.properties.get(EVIDENCE_REQUIRED)
        if wants:
            self.tasks.append(asyncio.ensure_future(self.fetch_evidence(obj.group)))

    async def fetch_evidence(self, group):
        """The result can arrive before its evidence is published, so INVALID_RANGE is retried."""
        deadline = time.monotonic() + self.fetch_window_s
        attempt = 0
        while True:
            attempt += 1
            self.record("fetch_request", group=group, attempt=attempt)
            try:
                f = await self.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(group, 0, 0))])
            except RequestFailed as failure:
                if failure.code not in (0x11, 0x9) or time.monotonic() >= deadline:
                    self.record("fetch_failed", group=group, code=failure.code, attempts=attempt)
                    return
                await asyncio.sleep(self.fetch_retry_s)
                continue
            except (asyncio.TimeoutError, ConnectionError) as failure:
                self.record("fetch_failed", group=group, code=None, reason=type(failure).__name__, attempts=attempt)
                return
            matching = [o for o in f.objects if o.group == group]
            if matching:
                for obj in matching:
                    self.object_record("fetch_object", 3, obj, attempts=attempt)
                return
            if f.reset is not None or time.monotonic() >= deadline:
                self.record("fetch_failed", group=group, code=f.reset, reason="not retained", attempts=attempt)
                return
            await asyncio.sleep(self.fetch_retry_s)

    async def run(self):
        try:
            # Extension X2, used only when the publisher stated support in SETUP.
            source = []
            if self.source_timeout_ms and self.peer_limits.get(w.OPT_SOURCE_TIMEOUT):
                source = [(w.P_SOURCE_TIMEOUT, self.source_timeout_ms)]
            subs = [await self.subscribe(NS, b"ai/events", [(w.P_PRIORITY, 64)] + source)]
            if self.evidence == "push":
                subs.append(await self.subscribe(NS, self.image_track, [(w.P_PRIORITY, 128)] + source))
            if self.status:
                subs.append(await self.subscribe(NS, b"status", [(w.P_PRIORITY, 200)]))
            await asyncio.gather(*(s.finished.wait() for s in subs))
            pending = [t for t in self.tasks[1:] if not t.done()]
            if pending:
                await asyncio.wait(pending, timeout=self.fetch_window_s)
            self.all_finished = True
            self.record("finished", objects=sum(len(s.objects) for s in subs), fetched=len(self.seen("fetch_object")))
            await asyncio.sleep(0.3)
            self.close(error_code=0)
        except (RequestFailed, asyncio.TimeoutError, ConnectionError) as failure:
            self.record("error", reason=str(failure))


async def main(args):
    args.out.mkdir(parents=True, exist_ok=True, mode=0o700)
    config = QuicConfiguration(
        is_client=False,
        alpn_protocols=[ALPN],
        max_datagram_frame_size=1200,
        max_data=2**21,
        max_stream_data=2**19,
        idle_timeout=20,
    )
    config.load_cert_chain(args.cert, args.key)
    server = await serve(
        args.host,
        args.port,
        configuration=config,
        create_protocol=lambda *a, **kw: ExperimentReceiver(
            *a,
            out=args.out,
            authority=args.authority,
            evidence=args.evidence,
            image_track=args.image_track.encode(),
            status=args.status,
            fetch_retry_s=args.fetch_retry,
            fetch_window_s=args.fetch_window,
            source_timeout_ms=args.source_timeout,
            extensions=not args.no_extensions,
            **kw,
        ),
    )
    try:
        await asyncio.sleep(args.seconds)
    finally:
        server.close()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=14568)
    p.add_argument("--authority", default="localhost:14568")
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=120)
    p.add_argument("--evidence", choices=["push", "fetch", "none"], default="fetch")
    p.add_argument("--image-track", default="evidence/jpeg")
    p.add_argument("--status", action="store_true", help="also subscribe to the status datagram track")
    p.add_argument("--fetch-retry", type=float, default=0.5)
    p.add_argument("--fetch-window", type=float, default=10.0)
    p.add_argument("--source-timeout", type=int, default=0, help="SOURCE_DELIVERY_TIMEOUT in ms (extension)")
    p.add_argument("--no-extensions", action="store_true", help="behave as a plain draft-21 subscriber")
    asyncio.run(main(p.parse_args()))
