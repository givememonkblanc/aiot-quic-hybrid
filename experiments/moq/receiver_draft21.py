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

import ssl

from aioquic.asyncio import connect, serve
from aioquic.quic.configuration import QuicConfiguration
from PIL import Image

import moqt_wire as w
from subscriber import ALPN, RequestFailed, Subscriber

NS = (b"aiot", b"esp32")
CAPTURE_MS, DETECTIONS, EVIDENCE_REQUIRED, REPRESENTATION = 0x78, 0x7A, 0x7C, 0x7E
PASSAGES_TOTAL, PASSAGES_NEW = 0x3800, 0x3802


class GapTracker:
    """Finds results that were published but did not arrive.

    Group IDs of the result track are source sequence numbers and therefore not
    contiguous. Each object states in PRIOR_GROUP_ID_GAP (draft-21 section 10.8) how many
    Groups before it do not exist, which identifies the Group that precedes it."""

    def __init__(self):
        self.received = set()
        self.frontier = -1  # every existing Group up to here has been received
        self.expected = {}  # Group known to exist but not received -> first time noticed
        self.gaps = {}  # received Group -> its PRIOR_GROUP_ID_GAP

    def add(self, group, gap, now):
        """Returns the newly expected Group, if this object reveals one."""
        self.received.add(group)
        self.expected.pop(group, None)
        found = None
        if gap is not None and gap <= group:
            previous = group - gap - 1
            if previous >= 0 and previous > self.frontier and previous not in self.received:
                if previous not in self.expected:
                    self.expected[previous] = now
                    found = previous
        self.advance()
        return found

    def advance(self):
        # The frontier moves over received Groups whose predecessor is already covered.
        moved = True
        while moved:
            moved = False
            for group in sorted(g for g in self.received if g > self.frontier):
                gap = self.gaps.get(group)
                if gap is None:
                    break
                previous = group - gap - 1
                if previous <= self.frontier:
                    self.frontier = group
                    moved = True
                else:
                    break

    def remember(self, group, gap):
        if gap is not None:
            self.gaps[group] = gap

    def due(self, now, grace):
        """Groups that stayed missing longer than the reordering grace period."""
        return sorted(g for g, since in self.expected.items() if now - since >= grace)
ALIASES = {b"preview/jpeg": 1, b"ai/events": 2, b"evidence/jpeg": 3, b"status": 4, b"helper/events": 5}
HELPER_ALIAS = 5  # helper/events of the relay (--helper-events): JSON detections of the helper ESP32


def describe(alias, payload):
    """Validates a payload and returns the fields logged with it."""
    if alias == HELPER_ALIAS:
        return dict(helper=json.loads(payload))
    if alias in (1, 3):
        im = Image.open(io.BytesIO(payload))
        im.load()
        if im.format != "JPEG":
            raise ValueError("not JPEG")
        return dict(width=im.width, height=im.height)
    if alias == 2 and payload[:1] == b"\xd1":
        # binary result (AIOT_MOQT_RESULT_BINARY): magic, version, backend, count, sizes,
        # sequence, score, then 6 bytes per detection: class, confidence, x, y, width, height
        if len(payload) < 20 or payload[1] != 2 or len(payload) != 20 + 6 * payload[3]:
            raise ValueError("binary result length")
        if payload[2] not in (2, 3, 4):
            raise ValueError("AI identity/backend")
        dets = [dict(zip(("class", "confidence", "x", "y", "w", "h"), payload[20 + 6 * i:26 + 6 * i]))
                for i in range(payload[3])]
        return dict(result=dict(schema=2, seq=int.from_bytes(payload[12:16], "little"), backend=payload[2],
                                detections=payload[3], boxes=dets))
    if alias == 2:
        result = json.loads(payload)
        if result.get("schema") != 1 or result.get("backend") not in (2, 3, 4):
            raise ValueError("AI identity/backend")
        return dict(result=result)
    return dict(status=json.loads(payload))


class ExperimentReceiver(Subscriber):
    def __init__(
        self,
        *a,
        out,
        authority,
        evidence,
        image_track,
        status,
        helper_events=False,
        fetch_retry_s,
        fetch_window_s,
        source_timeout_ms=0,
        recover=True,
        recover_grace_s=0.5,
        shared=None,
        probe_fetches=0,
        **kw,
    ):
        super().__init__(*a, log=self.write, **kw)
        self.source_timeout_ms = source_timeout_ms
        # `shared` outlives the connection, so a later session continues where this one stopped.
        self.probe_fetches = probe_fetches
        self.shared = shared if shared is not None else {}
        self.gaps = self.shared.setdefault("gaps", GapTracker())
        self.resumed = self.shared.get("sessions", 0) > 0
        self.shared["sessions"] = self.shared.get("sessions", 0) + 1
        self.recovering = False
        self.recover_grace_s = recover_grace_s
        self.recover = recover
        self.shared.setdefault("passages_total", 0)
        self.shared.setdefault("passage_events", 0)
        self.out = out
        self.authority = authority
        self.evidence = evidence
        self.image_track = image_track
        self.helper_events = helper_events
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

    @property
    def passage_events(self):
        return self.shared["passage_events"]

    @property
    def passages_total(self):
        return self.shared["passages_total"]

    def dispatch(self, e):
        super().dispatch(e)
        if self.terminated is not None and not self.all_finished:
            for task in self.tasks:
                task.cancel()
        if self.setup_done.is_set() and not self.started:
            self.started = True
            # a client checks no authority: the server sends none (section 9.3)
            if not self._quic.configuration.is_client and self.peer_setup.get(5) != self.authority.encode():
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
        # A relay assigns its own Track Aliases; the track name tells what an object is.
        subs = self.by_alias.get(obj.alias)
        if subs:
            obj.alias = ALIASES.get(bytes(subs[0].name), obj.alias)
        self.accept(obj, recovered=False)

    def accept(self, obj, recovered):
        """One result or image, from a subscription or from a recovery FETCH."""
        if obj.alias == 2 and obj.group in self.gaps.received:
            self.record("duplicate", alias=2, group=obj.group, recovered=recovered)
            return
        self.object_record("object", obj.alias, obj, recovered=recovered)
        if obj.alias != 2:
            return
        gap = obj.properties.get(w.X_PRIOR_GROUP_GAP)
        self.gaps.remember(obj.group, gap)
        missing = self.gaps.add(obj.group, gap, time.monotonic())
        self.shared["passage_events"] += obj.properties.get(PASSAGES_NEW, 0)
        self.shared["passages_total"] = max(self.shared["passages_total"], obj.properties.get(PASSAGES_TOTAL, 0))
        if missing is not None:
            self.record("gap_detected", alias=2, missing=missing, noticed_by=obj.group)
            if self.recover:
                self.tasks.append(asyncio.ensure_future(self.recover_results()))
        wants = self.evidence == "fetch" and obj.properties.get(EVIDENCE_REQUIRED)
        if wants:
            self.tasks.append(asyncio.ensure_future(self.fetch_evidence(obj.group)))

    async def probe(self, results):
        """Asks for several retained results at once and records how each request ends.

        With the limits of extension X1 the subscriber issues as many as the publisher
        stated; without them all requests leave together."""
        groups = sorted(self.gaps.received)[-self.probe_fetches:]
        started = time.monotonic()

        async def one(group):
            try:
                f = await self.fetch(NS, b"ai/events", [(w.P_LOCATION, w.location_filter(group, 0, 0))])
                return dict(group=group, objects=len(f.objects), code=None, s=time.monotonic() - started)
            except RequestFailed as failure:
                return dict(group=group, objects=0, code=failure.code, s=time.monotonic() - started)
            except (asyncio.TimeoutError, ConnectionError) as failure:
                return dict(group=group, objects=0, code=type(failure).__name__, s=time.monotonic() - started)

        outcomes = await asyncio.gather(*(one(g) for g in groups))
        self.record("fetch_probe", requested=len(groups), served=sum(o["objects"] > 0 for o in outcomes),
                    refused=sum(o["code"] is not None for o in outcomes), limits_known=bool(self.peer_limits),
                    outcomes=outcomes)

    async def recover_results(self, final=False):
        """Fetches the results between the frontier and the newest expected Group."""
        if self.recovering:
            return
        self.recovering = True
        try:
            while True:
                if not final:
                    await asyncio.sleep(self.recover_grace_s)  # the object may only be reordered
                due = self.gaps.due(time.monotonic(), 0 if final else self.recover_grace_s)
                if not due and not final:
                    return
                start = self.gaps.frontier + 1
                # The last request is open-ended: results after the newest known one count too.
                end = max(due) if due and not final else None
                fields = (start, 0) if end is None else (start, 0, end - start)
                if start == 0 and end is None:
                    fields = ()
                self.record("recover_request", alias=2, start=start, end=end, final=final)
                try:
                    f = await self.fetch(
                        NS, b"ai/events", [(w.P_LOCATION, w.location_filter(*fields))] if fields else []
                    )
                except RequestFailed as failure:
                    if failure.code != 0x11:  # INVALID_RANGE: nothing beyond the frontier
                        self.record("recover_failed", code=failure.code, start=start, end=end)
                    return
                except (asyncio.TimeoutError, ConnectionError) as failure:
                    self.record("recover_failed", code=None, reason=type(failure).__name__, start=start, end=end)
                    return
                fresh = 0
                for obj in f.objects:
                    obj.alias = 2
                    if obj.group not in self.gaps.received:
                        fresh += 1
                        self.accept(obj, recovered=True)
                # Expected Groups that the publisher no longer retains stay lost.
                for group in due:
                    if group not in self.gaps.received:
                        self.gaps.expected.pop(group, None)
                        self.record("lost", alias=2, group=group)
                self.record("recover_done", alias=2, start=start, end=end, objects=fresh, final=final)
                if final or not self.gaps.expected:
                    return
        finally:
            self.recovering = False

    async def fetch_evidence(self, group):
        """The result can arrive before its evidence is published, so INVALID_RANGE is retried."""
        deadline = time.monotonic() + self.fetch_window_s
        attempt = 0
        while True:
            attempt += 1
            self.record("fetch_request", group=group, attempt=attempt)
            try:
                f = await self.fetch(NS, self.image_track, [(w.P_LOCATION, w.location_filter(group, 0, 0))])
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
            results = await self.subscribe(NS, b"ai/events", [(w.P_PRIORITY, 64)] + source)
            subs = [results]
            if self.recover:
                self.hold(results)
                if self.resumed:
                    # A new session after the connection was lost: ask for everything since.
                    self.record("session_resumed", frontier=self.gaps.frontier)
                    self.tasks.append(asyncio.ensure_future(self.recover_results(final=True)))
            if self.evidence == "push":
                subs.append(await self.subscribe(NS, self.image_track, [(w.P_PRIORITY, 128)] + source))
            if self.status:
                subs.append(await self.subscribe(NS, b"status", [(w.P_PRIORITY, 200)]))
            if self.helper_events:
                # The helper's detections never end: not waited for with the other tracks.
                async def helper():
                    try:
                        await self.subscribe(NS, b"helper/events", [(w.P_PRIORITY, 64)])
                    except Exception as error:  # the relay may not serve it
                        self.record("helper_subscribe_failed", error=repr(error))
                self.tasks.append(asyncio.ensure_future(helper()))
            await asyncio.gather(*(s.complete.wait() for s in subs))
            if self.recover:
                # Results published last have no later object to reveal them: ask once more.
                while self.recovering:
                    await asyncio.sleep(0.05)
                await self.recover_results(final=True)
            pending = [t for t in self.tasks[1:] if not t.done()]
            if pending:
                await asyncio.wait(pending, timeout=self.fetch_window_s)
            if self.probe_fetches:
                await self.probe(results)
            if self.recover:
                self.release(results)
            await asyncio.gather(*(s.finished.wait() for s in subs))
            self.all_finished = True
            recovered = [e for e in self.events if e["event"] == "object" and e.get("recovered")]
            self.record(
                "finished",
                objects=sum(len(s.objects) for s in subs),
                fetched=len(self.seen("fetch_object")),
                results=len(self.gaps.received),
                recovered=len(recovered),
                lost=len(self.seen("lost")),
                passage_events=self.passage_events,
                passages_total=self.passages_total,
                sessions=self.shared["sessions"],
            )
            await asyncio.sleep(0.3)
            self.close(error_code=0)
        except asyncio.CancelledError:
            self.record("session_lost", results=len(self.gaps.received))
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
        idle_timeout=args.idle_timeout,
    )
    shared = {}  # results seen so far; a session after a lost connection continues from it

    def protocol(*a, **kw):
        return ExperimentReceiver(
            *a,
            out=args.out,
            authority=args.authority,
            evidence=args.evidence,
            image_track=args.image_track.encode(),
            status=args.status,
            helper_events=args.helper_events,
            fetch_retry_s=args.fetch_retry,
            fetch_window_s=args.fetch_window,
            source_timeout_ms=args.source_timeout,
            recover=not args.no_recover,
            recover_grace_s=args.recover_grace,
            extensions=not args.no_extensions,
            shared=shared,
            probe_fetches=args.probe_fetches,
            **kw,
        )

    if args.connect:
        await subscribe_through(args, config, protocol)
        return
    config.load_cert_chain(args.cert, args.key)
    server = await serve(args.host, args.port, configuration=config, create_protocol=protocol)
    try:
        await asyncio.sleep(args.seconds)
    finally:
        server.close()


async def subscribe_through(args, config, protocol):
    """Client mode: the subscriber connects to a relay. A lost connection is opened again
    after --reconnect seconds, and the new session recovers the results it missed."""
    host, port = args.connect.rsplit(":", 1)
    config.is_client = True
    config.verify_mode = ssl.CERT_NONE
    config.server_name = host
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        try:
            # The handshake is bounded by --connect-timeout: an attempt that meets an outage is
            # abandoned and started again, instead of waiting for the idle timeout.
            cm = connect(host, int(port), configuration=config, create_protocol=protocol, wait_connected=True)
            session = await asyncio.wait_for(cm.__aenter__(), args.connect_timeout or None)
            try:
                while time.monotonic() < end and session.terminated is None and not session.all_finished:
                    await asyncio.sleep(0.1)
                if session.all_finished:
                    return
            finally:
                await cm.__aexit__(None, None, None)
        except (ConnectionError, OSError, asyncio.TimeoutError) as failure:
            print(json.dumps(dict(t=time.monotonic(), event="connect_failed", reason=str(failure))), flush=True)
        await asyncio.sleep(args.reconnect)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=14568)
    p.add_argument("--authority", default="localhost:14568")
    p.add_argument("--cert")
    p.add_argument("--key")
    p.add_argument("--connect", help="host:port of a relay; the receiver is then a QUIC client")
    p.add_argument("--lenient-fetch-end", action="store_true",
                   help="take a FETCH_OK whose End Location precedes the Start Location as empty (subscriber.py)")
    p.add_argument("--reconnect", type=float, default=1.0, help="seconds before a lost relay connection is opened again")
    p.add_argument("--connect-timeout", type=float, default=0, help="longest handshake with the relay, seconds; 0: the idle timeout")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=120)
    p.add_argument("--evidence", choices=["push", "fetch", "none"], default="fetch")
    p.add_argument("--image-track", default="evidence/jpeg")
    p.add_argument("--status", action="store_true", help="also subscribe to the status datagram track")
    p.add_argument("--helper-events", action="store_true", help="also subscribe to helper/events (detections of the helper ESP32)")
    p.add_argument("--fetch-retry", type=float, default=0.5)
    p.add_argument("--fetch-window", type=float, default=10.0)
    p.add_argument("--source-timeout", type=int, default=0, help="SOURCE_DELIVERY_TIMEOUT in ms (extension)")
    p.add_argument("--no-extensions", action="store_true", help="behave as a plain draft-21 subscriber")
    p.add_argument("--probe-fetches", type=int, default=0, help="concurrent FETCH requests issued at the end")
    p.add_argument("--idle-timeout", type=float, default=20, help="QUIC idle timeout in seconds")
    p.add_argument("--no-recover", action="store_true", help="do not fetch results that did not arrive")
    p.add_argument("--recover-grace", type=float, default=0.5, help="seconds to wait for a reordered result")
    args = p.parse_args()
    Subscriber.lenient_fetch_end = args.lenient_fetch_end
    asyncio.run(main(args))
