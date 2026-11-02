"""MOQT draft-21 End Subscriber (QUIC server side).

Implements the subscriber half of the features listed in
papers/mission_aiot_moq/MOQT_DRAFT21_CHECKLIST_KO.md: parameterised SUBSCRIBE,
REQUEST_UPDATE, FETCH and fill fetch streams, TRACK_STATUS, namespace discovery,
publisher-initiated subscriptions, datagrams and GOAWAY. Not a relay.
"""

import asyncio
import time

from aioquic.asyncio import QuicConnectionProtocol
from aioquic.quic.events import (
    ConnectionTerminated,
    DatagramFrameReceived,
    HandshakeCompleted,
    StopSendingReceived,
    StreamDataReceived,
    StreamReset,
)

import moqt_wire as w

ALPN = "moqt-21"
MAX_OBJECT = 256 * 1024
IMPLEMENTATION = b"aiot-subscriber/2"


class LimitReached(Exception):
    """A limit stated by the peer would be exceeded; nothing was sent."""


class RequestFailed(Exception):
    def __init__(self, error):
        super().__init__(f"request error 0x{error['code']:x}: {error['reason']}")
        self.error = error
        self.code = error["code"]


def _varints(raw):
    out = []
    p = 0
    while p < len(raw):
        value, p = w.read_vi(raw, p)
        out.append(value)
    return out


def _ranges(raw, with_property):
    """Decodes a range filter value into (set_id, property_type, [(lo, hi)])."""
    set_id = raw[0]
    values = _varints(raw[1:])
    prop = values.pop(0) if with_property else None
    out = []
    prior = 0
    while values:
        lo = prior + values.pop(0)
        hi = lo + values.pop(0) if values else 2**64 - 1
        out.append((lo, hi))
        prior = hi
    return set_id, prop, out


class Subscription:
    """Subscriber-side view of one subscription, including its own filters.

    Several subscriptions to one track share a Track Alias, so the subscriber
    re-applies each filter to attribute a received Object (section 3.1)."""

    def __init__(self, stream, request_id, name, by_publish=False):
        self.stream = stream
        self.request_id = request_id
        self.name = name
        self.by_publish = by_publish
        self.forward = 1
        self.location = None
        self.window = (None, None)
        self.filters = []  # (parameter type, set_id, property, ranges)
        self.alias = None
        self.ok = None
        self.done = None
        self.objects = []
        self.streams = 0  # data streams seen, including reset and fill fetch streams
        self.resets = []
        self.finished = asyncio.Event()
        self.updates = []  # futures for outstanding REQUEST_UPDATEs
        self.pending_updates = []

    def apply(self, params, largest=None):
        replaced = {key for key, _ in params if 0x25 <= key <= 0x28}
        self.filters = [f for f in self.filters if f[0] not in replaced]
        for key, value in params:
            if key == w.P_FORWARD:
                self.forward = value
            elif key == w.P_LOCATION:
                self.location = _varints(value) or None
            elif 0x25 <= key <= 0x28 and len(value):
                self.filters.append((key, *_ranges(bytes(value), key == w.P_OBJECT_PROPERTY_FILTER)))
        if largest is not None or self.window == (None, None):
            self.resolve(largest)

    def resolve(self, largest):
        f = self.location
        if not f:
            self.window = (None, None)
            return
        if len(f) == 1:
            next_group = largest[0] + 1 if largest else 0
            start = (max(0, next_group - f[0]), 0)
        elif len(f) == 2 and f == [0, 0]:
            start = (largest[0], largest[1] + 1) if largest else (0, 0)
        else:
            start = (f[0], f[1])
        end = None
        if len(f) >= 3:
            end = (f[0] + f[2], f[3] if len(f) == 4 else None)
        self.window = (start, end)

    def accepts(self, obj):
        if not self.forward:
            return False
        start, end = self.window
        at = (obj.group, obj.object)
        if start is not None and at < start:
            return False
        if end is not None and (obj.group > end[0] or (obj.group == end[0] and end[1] is not None and obj.object > end[1])):
            return False
        if not self.filters:
            return True
        for set_id in sorted({f[1] for f in self.filters}):
            matched = True
            for key, sid, prop, ranges in self.filters:
                if sid != set_id:
                    continue
                if key == w.P_SUBGROUP_FILTER:
                    value = obj.subgroup
                elif key == w.P_OBJECTID_FILTER:
                    value = obj.object
                elif key == w.P_PRIORITY_FILTER:
                    value = obj.priority if obj.priority is not None else self.default_priority()
                else:
                    value = obj.properties.get(prop)
                if value is None or not any(lo <= value <= hi for lo, hi in ranges):
                    matched = False
                    break
            if matched:
                return True
        return False

    def default_priority(self):
        return (self.ok or {}).get("properties", {}).get(w.X_PRIORITY, 128)


class Fetch:
    def __init__(self, stream, request_id, name, descending):
        self.stream = stream
        self.request_id = request_id
        self.name = name
        self.ok = None
        self.reader = w.FetchReader(descending)
        self.objects = []
        self.complete = asyncio.Event()
        self.reset = None
        self.data_stream = None
        self.updates = []


class Subscriber(QuicConnectionProtocol):
    def __init__(
        self,
        *a,
        max_filter_ranges=8,
        max_request_updates=0,
        accept_publish=True,
        log=None,
        extensions=True,
        own_limits=None,
        **kw,
    ):
        super().__init__(*a, **kw)
        self.extensions = extensions  # honour and advertise the proposed extensions
        self.own_limits = own_limits or {}  # extension options this endpoint states
        self.peer_limits = {}
        self.fetch_gate = None
        self.local_refusals = 0
        self.max_filter_ranges = max_filter_ranges
        self.max_request_updates = max_request_updates
        self.accept_publish = accept_publish
        self.publish_response = None  # parameters for PUBLISH_OK, or an error code
        self.log_sink = log
        self.buffers = {}
        self.fins = set()
        self.control = None
        self.peer_control = None
        self.peer_setup = None
        self.next_request = 1  # the server uses odd Request IDs (section 6.4.2.1)
        self.requests = {}  # stream -> handler state
        self.waiting = {}  # stream -> future for the first response
        self.subscriptions = {}  # request_id -> Subscription
        self.by_alias = {}  # alias -> [Subscription]
        self.fetches = {}  # request_id -> Fetch
        self.data = {}  # unidirectional stream -> decoder state
        self.datagrams = []
        self.namespaces = {}  # stream -> dict(prefix, active, done, publishes, skipped)
        self.published_namespaces = []
        self.goaways = []
        self.events = []
        self.errors = []
        self.terminated = None
        self.setup_done = asyncio.Event()
        self.changed = asyncio.Event()

    # -- logging and waiting -------------------------------------------------
    def note(self, event, **kw):
        record = dict(t=time.monotonic(), event=event, **kw)
        self.events.append(record)
        if self.log_sink:
            self.log_sink(record)
        self.changed.set()

    def fail(self, reason, code=w.PROTOCOL_VIOLATION):
        self.errors.append(reason)
        self.note("error", reason=reason, code=code)
        self.close(error_code=code, reason_phrase=reason[:100])

    async def until(self, predicate, timeout=5.0):
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while not predicate():
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise TimeoutError("subscriber condition not reached")
            self.changed.clear()
            try:
                await asyncio.wait_for(self.changed.wait(), min(remaining, 0.05))
            except asyncio.TimeoutError:
                pass

    # -- QUIC events -----------------------------------------------------------
    def quic_event_received(self, e):
        try:
            self.dispatch(e)
        except w.Violation as v:
            self.fail(str(v), v.code)
        except (ValueError, KeyError) as v:
            self.fail(str(v))

    def dispatch(self, e):
        if isinstance(e, HandshakeCompleted):
            if e.alpn_protocol != ALPN:
                self.fail("ALPN")
                return
            self.note("handshake", alpn=e.alpn_protocol)
            self.control = self._quic.get_next_available_stream_id(is_unidirectional=True)
            options = [(0x07, IMPLEMENTATION)]
            if self.max_filter_ranges:
                options.append((0x06, self.max_filter_ranges))
            if self.max_request_updates:
                options.append((0x08, self.max_request_updates))
            if self.extensions:
                options.append((w.OPT_SOURCE_TIMEOUT, 1))
                options.extend(self.own_limits.items())
            self._quic.send_stream_data(self.control, w.setup(options))
            self.transmit()
        elif isinstance(e, StreamDataReceived):
            buf = self.buffers.setdefault(e.stream_id, bytearray())
            buf.extend(e.data)
            if e.end_stream:
                self.fins.add(e.stream_id)
            self.consume(e.stream_id)
            self.transmit()
        elif isinstance(e, StreamReset):
            self.on_reset(e.stream_id, e.error_code)
        elif isinstance(e, StopSendingReceived):
            self.note("stop_sending", stream=e.stream_id, code=e.error_code)
        elif isinstance(e, DatagramFrameReceived):
            obj = w.read_datagram(e.data)
            if obj is not None:
                self.deliver(obj, None)
                self.datagrams.append(obj)
        elif isinstance(e, ConnectionTerminated):
            self.terminated = dict(code=e.error_code, reason=e.reason_phrase)
            self.note("closed", code=e.error_code, reason=e.reason_phrase)
            for future in list(self.waiting.values()):
                if not future.done():
                    future.set_exception(ConnectionError("session closed"))

    def on_reset(self, stream, code):
        state = self.data.pop(stream, None)
        self.buffers.pop(stream, None)
        if state is None:
            # The header may be sitting in the buffer that the reset discarded.
            self.note("reset", stream=stream, code=code, alias=None, group=None)
            if stream in self.requests:
                self.request_closed(stream, code)
            return
        if state["kind"] == "fetch":
            f = state["fetch"]
            f.reset = code
            f.complete.set()
            self.note("fetch_reset", request=f.request_id, code=code)
            if state.get("fill"):
                state["fill"].streams += 1
            return
        header = state["header"]
        for s in self.by_alias.get(header.alias, []):
            s.streams += 1
            s.resets.append(dict(group=header.group, code=code))
            self.check_done(s)
        self.note("reset", stream=stream, code=code, alias=header.alias, group=header.group)

    def request_closed(self, stream, code):
        state = self.requests.pop(stream, None)
        future = self.waiting.pop(stream, None)
        if future and not future.done():
            future.set_exception(ConnectionError(f"request stream reset 0x{code:x}"))
        if state and state["kind"] == "subscription":
            state["sub"].finished.set()
        if state and state["kind"] == "namespace":
            # Section 9.15: a reset or FIN ends every active namespace.
            state["active"].clear()

    # -- stream consumption ------------------------------------------------------
    def consume(self, sid):
        b = self.buffers[sid]
        low = sid & 3
        if low == 2:  # client-initiated unidirectional
            self.consume_uni(sid, b)
        elif low == 1:  # own request streams
            self.consume_response(sid, b)
        elif low == 0:  # requests from the publisher
            self.consume_request(sid, b)

    def messages(self, sid, b):
        while b:
            try:
                kind, body, n = w.read_message(b)
            except w.NeedMore:
                if sid in self.fins:
                    raise w.Violation("stream ended inside a message") from None
                return
            del b[:n]
            yield kind, body

    def consume_uni(self, sid, b):
        state = self.data.get(sid)
        if state is None and sid != self.peer_control:
            try:
                kind, _ = w.read_vi(b)
            except w.NeedMore:
                return
            if kind == w.SETUP:
                if self.peer_control is not None:
                    raise w.Violation("second control stream")
                self.peer_control = sid
            elif kind == w.PADDING_STREAM:
                state = self.data[sid] = dict(kind="padding")
            elif kind == w.FETCH_HEADER:
                state = self.data[sid] = dict(kind="fetch", fetch=None, at=0)
            elif w.is_subgroup_type(kind):
                state = self.data[sid] = dict(kind="subgroup", header=None, previous=None, at=0)
            else:
                raise w.Violation(f"unknown stream type 0x{kind:x}")
        if sid == self.peer_control:
            if sid in self.fins:
                raise w.Violation("control stream closed")
            for kind, body in self.messages(sid, b):
                if self.peer_setup is None:
                    if kind != w.SETUP:
                        raise w.Violation("first control message is not SETUP")
                    self.peer_setup = w.setup_fields(body)
                    if 1 not in self.peer_setup or 5 not in self.peer_setup:
                        raise w.Violation("client SETUP without PATH or AUTHORITY")
                    if self.extensions:
                        self.peer_limits = {
                            k: self.peer_setup[k]
                            for k in (
                                w.OPT_MAX_CONTROL_MESSAGE,
                                w.OPT_MAX_SUBSCRIPTIONS,
                                w.OPT_MAX_FETCHES,
                                w.OPT_SOURCE_TIMEOUT,
                            )
                            if k in self.peer_setup
                        }
                        if w.OPT_MAX_FETCHES in self.peer_limits:
                            self.fetch_gate = asyncio.Semaphore(max(1, self.peer_limits[w.OPT_MAX_FETCHES]))
                    self.note(
                        "setup",
                        implementation=self.peer_setup.get(7, b"").decode("utf-8", "replace"),
                        limits={hex(k): v for k, v in self.peer_limits.items()},
                    )
                    self.setup_done.set()
                elif kind == w.GOAWAY:
                    g = w.parse(kind, body)
                    if g["uri"]:
                        raise w.Violation("a client may not name a new session URI")
                    if any(x["stream"] == sid for x in self.goaways):
                        raise w.Violation("second GOAWAY")
                    self.goaways.append(dict(stream=sid, **g))
                    self.note("goaway", timeout=g["timeout"])
                else:
                    raise w.Violation(f"message 0x{kind:x} on the control stream")
            return
        if state["kind"] == "padding":
            if any(b):
                raise w.Violation("non-zero padding")
            b.clear()
        elif state["kind"] == "subgroup":
            self.consume_subgroup(sid, b, state)
        else:
            self.consume_fetch(sid, b, state)

    def consume_subgroup(self, sid, b, state):
        if len(b) > MAX_OBJECT + 4096:
            raise w.Violation("object size limit")
        try:
            if state["header"] is None:
                state["header"] = w.read_subgroup_header(b)
                state["at"] = state["header"].size
            while state["at"] < len(b):
                obj, p = w.read_subgroup_object(b, state["at"], state["header"], state["previous"])
                state["at"] = p
                state["previous"] = obj.object
                self.deliver(obj, sid)
        except w.NeedMore:
            if sid in self.fins:
                raise w.Violation("subgroup stream ended inside an object") from None
            return
        if sid in self.fins:
            header = state["header"]
            if header.end_of_group and state["previous"] is not None:
                self.note("end_of_group", alias=header.alias, group=header.group, last=state["previous"])
            for s in self.by_alias.get(header.alias, []):
                s.streams += 1
                self.check_done(s)
            del self.data[sid]
            del self.buffers[sid]

    def consume_fetch(self, sid, b, state):
        try:
            if state["fetch"] is None:
                reader = w.FetchReader()
                at = reader.header(b)
                f = self.fetches.get(reader.request_id)
                if f is None:
                    raise w.Violation("fetch stream for an unknown request")
                f.reader.request_id = reader.request_id
                f.data_stream = sid
                state["fetch"] = f
                state["fill"] = f.fill_for if hasattr(f, "fill_for") else None
                state["at"] = at
            f = state["fetch"]
            while state["at"] < len(b):
                obj, p = f.reader.next(b, state["at"])
                state["at"] = p
                if obj is not None:
                    f.objects.append(obj)
                    self.note(
                        "fetch_object",
                        request=f.request_id,
                        group=obj.group,
                        object=obj.object,
                        bytes=len(obj.payload),
                    )
        except w.NeedMore:
            if sid in self.fins:
                raise w.Violation("fetch stream ended inside an object") from None
            return
        if sid in self.fins:
            f = state["fetch"]
            f.complete.set()
            if state["fill"] is not None:
                state["fill"].streams += 1
                self.check_done(state["fill"])
            self.note("fetch_complete", request=f.request_id, objects=len(f.objects))
            del self.data[sid]
            del self.buffers[sid]

    def deliver(self, obj, sid):
        targets = self.by_alias.get(obj.alias)
        if not targets:
            # Section 11.3.1: objects may precede the message that establishes the alias.
            self.note("unknown_alias", alias=obj.alias, group=obj.group)
            self.pending = getattr(self, "pending", [])
            self.pending.append(obj)
            return
        # The publisher sends one copy per matching subscription; give each copy to the
        # first matching subscription that does not hold this Location yet.
        matching = [s for s in targets if s.accepts(obj)] or targets[:1]
        holder = next(
            (s for s in matching if all((o.group, o.object) != (obj.group, obj.object) for o in s.objects)),
            None,
        )
        if holder is None:
            self.note("duplicate", alias=obj.alias, group=obj.group, object=obj.object)
            return
        holder.objects.append(obj)
        self.note(
            "object",
            alias=obj.alias,
            group=obj.group,
            object=obj.object,
            subgroup=obj.subgroup,
            priority=obj.priority,
            status=obj.status,
            datagram=obj.datagram,
            properties={str(k): v for k, v in obj.properties.items() if isinstance(v, int)},
            bytes=len(obj.payload),
            stream=sid,
        )

    def bind_alias(self, sub, alias):
        sub.alias = alias
        self.by_alias.setdefault(alias, []).append(sub)
        for obj in [o for o in getattr(self, "pending", []) if o.alias == alias]:
            self.pending.remove(obj)
            sub.objects.append(obj)

    def check_done(self, sub):
        if sub.done is not None and sub.streams >= sub.done["streams"] and not sub.finished.is_set():
            sub.finished.set()
            self._quic.send_stream_data(sub.stream, b"", end_stream=True)
            self.note("track_finished", alias=sub.alias, request=sub.request_id, streams=sub.streams)

    # -- responses to own requests -----------------------------------------------
    def consume_response(self, sid, b):
        state = self.requests.get(sid)
        if state is None:
            b.clear()
            return
        for kind, body in self.messages(sid, b):
            self.on_response(sid, state, kind, body)
        if sid in self.fins and not state.get("closed"):
            state["closed"] = True
            if state["kind"] == "namespace":
                state["active"].clear()
            # Section 6.4.2.2: once the responder finishes, the requester closes its half.
            if not (state["kind"] == "subscription" and state["sub"].finished.is_set()):
                self._quic.send_stream_data(sid, b"", end_stream=True)
            self.note("request_stream_finished", stream=sid, kind=state["kind"])

    def resolve(self, sid, value=None, error=None):
        future = self.waiting.pop(sid, None)
        if future and not future.done():
            if error is not None:
                future.set_exception(RequestFailed(error))
            else:
                future.set_result(value)

    def on_response(self, sid, state, kind, body):
        first = not state.get("answered")
        if kind == w.GOAWAY:
            g = w.parse(kind, body)
            if state.get("goaway"):
                raise w.Violation("second GOAWAY on a request stream")
            state["goaway"] = g
            self.goaways.append(dict(stream=sid, **g))
            return
        if kind == w.REQUEST_ERROR:
            error = w.parse(kind, body)
            self.note("request_error", stream=sid, code=error["code"], reason=error["reason"])
            if first:
                state["answered"] = True
                state["failed"] = True
                if state.get("sub") is not None:
                    state["sub"].finished.set()
                self.resolve(sid, error=error)
                return
            self.resolve_update(state, error=error)
            return
        if first:
            state["answered"] = True
            expected = w.SUBSCRIBE_OK if state["kind"] == "subscription" else w.FETCH_OK if state["kind"] == "fetch" else w.REQUEST_OK
            if kind != expected:
                raise w.Violation(f"unexpected first response 0x{kind:x}")
            ok = w.parse(kind, body)
            if state["kind"] == "subscription":
                sub = state["sub"]
                if self.unsupported(ok["properties"]):
                    # Section 3.6: an unknown Mandatory Track Property forbids processing.
                    self.cancel(sid)
                    self.note("unsupported_extension", track=state["sub"].name.decode())
                    self.resolve(sid, error=dict(code=0x33, retry=0, reason="mandatory track property"))
                    return
                sub.ok = ok
                sub.resolve(ok["params"].get(w.P_LARGEST))
                self.bind_alias(sub, ok["alias"])
                self.note("subscribed", alias=ok["alias"], track=sub.name.decode(), request=sub.request_id)
                self.resolve(sid, sub)
            elif state["kind"] == "fetch":
                f = state["fetch"]
                f.ok = ok
                start = state.get("start")
                if start is not None and ok["end"] < start:
                    raise w.Violation("FETCH_OK End Location precedes the Start Location")
                self.resolve(sid, f)
            else:
                if state["kind"] != "status" and ok["properties"]:
                    raise w.Violation("Track Properties outside TRACK_STATUS_OK")
                self.resolve(sid, ok)
            return
        if kind == w.REQUEST_OK:
            ok = w.parse(kind, body)
            if state.get("sub") is not None and state["sub"].pending_updates:
                state["sub"].apply(state["sub"].pending_updates.pop(0), ok["params"].get(w.P_LARGEST))
            self.resolve_update(state, value=ok)
        elif kind == w.PUBLISH_DONE and state["kind"] == "subscription":
            sub = state["sub"]
            sub.done = w.parse(kind, body)
            self.note(
                "publish_done",
                alias=sub.alias,
                status=sub.done["status"],
                streams=sub.done["streams"],
                reason=sub.done["reason"],
            )
            shared = len(self.by_alias.get(sub.alias, [])) > 1
            if not shared and sub.streams > sub.done["streams"]:
                # With a shared Track Alias the streams of several subscriptions are
                # indistinguishable, so the exact comparison applies to a sole subscription.
                raise w.Violation("more streams than the Stream Count")
            self.check_done(sub)
        elif kind == w.PUBLISH_STATE_NOTIFY and state["kind"] == "subscription":
            state["sub"].notifications = getattr(state["sub"], "notifications", []) + [w.parse(kind, body)]
        elif kind in (w.NAMESPACE, w.NAMESPACE_DONE) and state["kind"] == "namespace":
            suffix = w.parse(kind, body)
            if kind == w.NAMESPACE:
                state["active"].add(suffix)
                state["seen"].append(suffix)
            else:
                if suffix not in state["active"]:
                    raise w.Violation("NAMESPACE_DONE before NAMESPACE")
                state["active"].discard(suffix)
                state["done"].append(suffix)
            self.note("namespace" if kind == w.NAMESPACE else "namespace_done", suffix=[x.decode() for x in suffix])
        elif kind == w.PUBLISH_SKIPPED and state["kind"] == "tracks":
            state["skipped"].append(w.parse(kind, body))
        else:
            raise w.Violation(f"message 0x{kind:x} is not valid on this request stream")

    known_mandatory = frozenset()

    def unsupported(self, props):
        return any(0x4000 <= key <= 0x7FFF and key not in self.known_mandatory for key in props)

    def resolve_update(self, state, value=None, error=None):
        owner = state.get("sub") or state.get("fetch") or state
        pending = owner.updates if hasattr(owner, "updates") else state.setdefault("updates", [])
        if not pending:
            raise w.Violation("response without an outstanding REQUEST_UPDATE")
        future = pending.pop(0)
        if error is not None:
            future.set_exception(RequestFailed(error))
        else:
            future.set_result(value)

    # -- requests from the publisher ----------------------------------------------
    def consume_request(self, sid, b):
        state = self.requests.get(sid)
        for kind, body in self.messages(sid, b):
            if state is None:
                if kind == w.PUBLISH:
                    state = self.on_publish(sid, w.parse(kind, body))
                elif kind == w.PUBLISH_NAMESPACE:
                    p = w.parse(kind, body)
                    self.check_peer_request(p["request_id"])
                    self.published_namespaces.append(p["namespace"])
                    self.note("publish_namespace", namespace=[x.decode() for x in p["namespace"]])
                    self._quic.send_stream_data(sid, w.request_ok())
                    state = self.requests[sid] = dict(kind="published_namespace", answered=True)
                else:
                    raise w.Violation(f"request stream begins with 0x{kind:x}")
            elif state["kind"] == "subscription":
                self.on_response(sid, state, kind, body)
            else:
                raise w.Violation("message after the request on a publisher stream")

    def check_peer_request(self, request_id):
        if request_id & 1:
            raise w.Violation("client Request IDs are even", 0x4)
        seen = self.__dict__.setdefault("peer_requests", set())
        if request_id in seen:
            raise w.Violation("duplicate Request ID", 0x4)
        seen.add(request_id)

    def on_publish(self, sid, p):
        self.check_peer_request(p["request_id"])
        self.note("publish", track=p["name"].decode(), alias=p["alias"], params={str(k): str(v) for k, v in p["params"].items()})
        if not self.accept_publish or isinstance(self.publish_response, int):
            code = self.publish_response if isinstance(self.publish_response, int) else 0x20
            self._quic.send_stream_data(sid, w.request_error(code, 0, b"declined"), end_stream=True)
            state = self.requests[sid] = dict(kind="declined", answered=True)
            return state
        sub = Subscription(sid, p["request_id"], p["name"], by_publish=True)
        sub.ok = p
        sub.apply(list(p["params"].items()) and [(k, v) for k, v in p["params"].items() if k in (w.P_FORWARD, w.P_LOCATION)])
        sub.apply(list(self.publish_response or ()), p["params"].get(w.P_LARGEST))
        for other in self.by_alias.get(p["alias"], []):
            if other.name != p["name"]:
                raise w.Violation("duplicate track alias", 0x5)
        self.bind_alias(sub, p["alias"])
        self.subscriptions[("publish", p["request_id"])] = sub
        self._quic.send_stream_data(sid, w.request_ok(self.publish_response or ()))
        state = self.requests[sid] = dict(kind="subscription", sub=sub, answered=True)
        self.note("subscribed", alias=p["alias"], track=p["name"].decode(), request=p["request_id"], by_publish=True)
        return state

    # -- request API -------------------------------------------------------------------
    def take_request_id(self):
        request_id = self.next_request
        self.next_request += 2
        return request_id

    def check_limits(self, message, kind):
        """Refuses locally what the peer said it cannot serve (extension X1)."""
        limit = self.peer_limits.get(w.OPT_MAX_CONTROL_MESSAGE)
        _, body, _ = w.read_message(message, limit=65535)
        if limit is not None and len(body) > limit:
            self.local_refusals += 1
            raise LimitReached(f"control message of {len(body)} bytes exceeds the peer limit {limit}")
        limit = self.peer_limits.get(w.OPT_MAX_SUBSCRIPTIONS)
        if kind == "subscription" and limit is not None:
            active = sum(1 for s in self.subscriptions.values() if s.done is None and not s.finished.is_set())
            if active >= limit:
                self.local_refusals += 1
                raise LimitReached(f"the peer serves {limit} subscriptions")

    def open_request(self, message, state, request_id=None):
        self.check_limits(message, state["kind"])
        sid = self._quic.get_next_available_stream_id()
        self.requests[sid] = state
        future = asyncio.get_running_loop().create_future()
        self.waiting[sid] = future
        self._quic.send_stream_data(sid, message)
        self.transmit()
        return sid, future

    async def subscribe(self, ns, name, params=(), request_id=None, timeout=5.0):
        request_id = self.take_request_id() if request_id is None else request_id
        if any(k == w.P_SOURCE_TIMEOUT for k, _ in params) and not self.peer_limits.get(w.OPT_SOURCE_TIMEOUT):
            # An unknown parameter would end the session (section 9.20), so it is not sent.
            raise LimitReached("the peer did not state support for SOURCE_DELIVERY_TIMEOUT")
        state = dict(kind="subscription")
        sid, future = self.open_request(w.subscribe(request_id, ns, name, params), state)
        state["sub"] = sub = Subscription(sid, request_id, name)
        sub.apply(list(params))
        self.subscriptions[request_id] = sub
        for key, value in params:
            if key == w.P_FILL:
                self.expect_fill(sub, request_id, value)
        return await asyncio.wait_for(future, timeout)

    def expect_fill(self, sub, request_id, raw):
        descending = False
        p = 0
        prev = 0
        while p < len(raw):
            delta, p = w.read_vi(raw, p)
            key = prev + delta
            prev = key
            if key == w.P_GROUP_ORDER:
                descending = raw[p] == w.DESCENDING
                p += 1
            elif key in (w.P_PRIORITY,):
                p += 1
            elif key == w.P_FILL_TIMEOUT:
                _, p = w.read_vi(raw, p)
            else:
                n, p = w.read_vi(raw, p)
                p += n
        f = Fetch(None, request_id, sub.name, descending)
        f.fill_for = sub
        self.fetches[request_id] = f
        sub.fills = getattr(sub, "fills", []) + [f]
        return f

    async def update(self, target, params=(), request_id=None, timeout=5.0):
        """REQUEST_UPDATE on the request stream of a subscription, fetch or namespace request."""
        request_id = self.take_request_id() if request_id is None else request_id
        sid = target if isinstance(target, int) else target.stream
        future = asyncio.get_running_loop().create_future()
        state = self.requests[sid]
        owner = state.get("sub") or state.get("fetch")
        (owner.updates if owner is not None else state.setdefault("updates", [])).append(future)
        if state.get("sub") is not None:
            state["sub"].pending_updates.append(list(params))
            for key, value in params:
                if key == w.P_FILL:
                    self.expect_fill(state["sub"], request_id, value)
        self._quic.send_stream_data(sid, w.request_update(request_id, params))
        self.transmit()
        return await asyncio.wait_for(future, timeout)

    async def fetch(self, ns, name, params=(), request_id=None, timeout=5.0):
        """With a fetch limit stated by the peer, requests wait for a free place instead of failing."""
        if self.fetch_gate is None:
            return await self._fetch(ns, name, params, request_id, timeout)
        async with self.fetch_gate:
            return await self._fetch(ns, name, params, request_id, timeout)

    async def _fetch(self, ns, name, params=(), request_id=None, timeout=5.0):
        request_id = self.take_request_id() if request_id is None else request_id
        descending = any(k == w.P_GROUP_ORDER and v == w.DESCENDING for k, v in params)
        state = dict(kind="fetch")
        for key, value in params:
            if key == w.P_LOCATION and len(value):
                fields = []
                p = 0
                while p < len(value):
                    x, p = w.read_vi(value, p)
                    fields.append(x)
                if len(fields) >= 2:
                    state["start"] = (fields[0], fields[1])
        f = Fetch(None, request_id, name, descending)
        self.fetches[request_id] = f
        state["fetch"] = f
        sid, future = self.open_request(w.fetch(request_id, ns, name, params), state)
        f.stream = sid
        await asyncio.wait_for(future, timeout)
        await asyncio.wait_for(f.complete.wait(), timeout)
        return f

    async def track_status(self, ns, name, params=(), timeout=5.0):
        _, future = self.open_request(w.track_status(self.take_request_id(), ns, name, params), dict(kind="status"))
        return await asyncio.wait_for(future, timeout)

    async def subscribe_namespace(self, prefix, params=(), timeout=5.0):
        state = dict(kind="namespace", prefix=tuple(prefix), active=set(), seen=[], done=[])
        sid, future = self.open_request(w.subscribe_namespace(self.take_request_id(), prefix, params), state)
        await asyncio.wait_for(future, timeout)
        state["stream"] = sid
        return state

    async def subscribe_tracks(self, prefix, params=(), timeout=5.0):
        state = dict(kind="tracks", prefix=tuple(prefix), skipped=[])
        sid, future = self.open_request(w.subscribe_tracks(self.take_request_id(), prefix, params), state)
        await asyncio.wait_for(future, timeout)
        state["stream"] = sid
        return state

    def cancel(self, target):
        """Section 6.4.2.3: reset the sending half and stop the receiving half."""
        sid = target if isinstance(target, int) else target.stream
        self._quic.reset_stream(sid, 0x1)
        self._quic.stop_stream(sid, 0x1)
        state = self.requests.get(sid)
        if state and state.get("sub") is not None:
            sub = state["sub"]
            sub.finished.set()
            if sub in self.by_alias.get(sub.alias, []):
                self.by_alias[sub.alias].remove(sub)
        self.transmit()

    def stop_data_stream(self, sid):
        self._quic.stop_stream(sid, 0x1)
        self.transmit()

    def send_goaway(self, timeout=0, stream=None):
        self._quic.send_stream_data(self.control if stream is None else stream, w.goaway(b"", timeout))
        self.transmit()

    def send_raw(self, stream, data, end=False):
        self._quic.send_stream_data(stream, data, end_stream=end)
        self.transmit()

    def new_request_stream(self):
        return self._quic.get_next_available_stream_id()

    def seen(self, event):
        return [e for e in self.events if e["event"] == event]
