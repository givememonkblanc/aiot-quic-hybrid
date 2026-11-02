"""Publisher that drives the portable C core (src/components/transport/moqt_core.c)
over a real QUIC connection. The MCU adapter uses the same core, so host runs
exercise the protocol logic that ships in the firmware."""

import asyncio
import ctypes
import subprocess
import tempfile
import time
from pathlib import Path

from aioquic.asyncio import QuicConnectionProtocol
from aioquic.quic.events import (
    ConnectionTerminated,
    HandshakeCompleted,
    StopSendingReceived,
    StreamDataReceived,
    StreamReset,
)

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
TRANSPORT = ROOT / "src/components/transport"
HEAD = 128
(
    ACT_NONE,
    ACT_SEND,
    ACT_OPEN_BIDI,
    ACT_FETCH_START,
    ACT_FETCH_CANCEL,
    ACT_FILL_FAILED,
    ACT_SUB_ENDING,
    ACT_SUB_CANCELLED,
    ACT_RESET_STREAM,
    ACT_STOP_SENDING,
    ACT_CLOSE_SESSION,
    ACT_SUBSCRIBED,
    ACT_UPDATED,
    ACT_NEW_GROUP,
    ACT_GOAWAY,
    ACT_PEER_FIN,
    ACT_NAMESPACE_RESULT,
    ACT_REQUEST_CLOSED,
) = range(18)
RST_DELIVERY_TIMEOUT = 0x2
RST_CANCELLED = 0x1


class TrackCfg(ctypes.Structure):
    _fields_ = [
        ("ns", ctypes.c_char_p * 4),
        ("ns_count", ctypes.c_uint8),
        ("name", ctypes.c_char_p),
        ("priority", ctypes.c_uint8),
        ("group_order", ctypes.c_uint8),
        ("object_timeout_ms", ctypes.c_uint32),
        ("subgroup_timeout_ms", ctypes.c_uint32),
        ("max_cache_ms", ctypes.c_uint32),
        ("dynamic_groups", ctypes.c_bool),
        ("source_timeout_ms", ctypes.c_uint32),
    ]


class Action(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("stream", ctypes.c_int64),
        ("code", ctypes.c_uint64),
        ("index", ctypes.c_uint),
        ("track", ctypes.c_uint),
        ("token", ctypes.c_uint),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("len", ctypes.c_size_t),
        ("fin", ctypes.c_bool),
    ]


class Offer(ctypes.Structure):
    _fields_ = [
        ("group", ctypes.c_uint64),
        ("object", ctypes.c_uint64),
        ("subgroup", ctypes.c_uint64),
        ("priority", ctypes.c_uint8),
        ("status", ctypes.c_uint8),
        ("has_priority", ctypes.c_bool),
        ("end_of_group", ctypes.c_bool),
        ("datagram", ctypes.c_bool),
        ("props", ctypes.c_char_p),
        ("props_len", ctypes.c_size_t),
        ("payload_len", ctypes.c_size_t),
        ("has_source_age", ctypes.c_bool),
        ("source_age_ms", ctypes.c_uint64),
    ]


class Delivery(ctypes.Structure):
    _fields_ = [
        ("sub", ctypes.c_uint),
        ("alias", ctypes.c_uint64),
        ("request_id", ctypes.c_uint64),
        ("subscriber_priority", ctypes.c_uint8),
        ("publisher_priority", ctypes.c_uint8),
        ("group_order", ctypes.c_uint8),
        ("datagram", ctypes.c_bool),
        ("object_timeout_ms", ctypes.c_uint64),
        ("subgroup_timeout_ms", ctypes.c_uint64),
        ("source_timeout_ms", ctypes.c_uint64),
        ("source_age_ms", ctypes.c_uint64),
        ("head", ctypes.c_uint8 * HEAD),
        ("head_len", ctypes.c_size_t),
    ]


_LIB = None


def library():
    """Builds the shared library once per process, with sanitizer-free flags."""
    global _LIB
    if _LIB is not None:
        return _LIB
    directory = Path(tempfile.mkdtemp(prefix="moqt-core-"))
    target = directory / "libmoqtcore.so"
    subprocess.run(
        [
            "gcc",
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-shared",
            "-fPIC",
            f"-I{TRANSPORT}",
            str(TRANSPORT / "moqt21.c"),
            str(TRANSPORT / "moqt_core.c"),
            str(BASE / "host_core_shim.c"),
            "-o",
            str(target),
        ],
        check=True,
    )
    lib = ctypes.CDLL(str(target))
    sizes = dict(
        action=(lib.moqt_action_sizeof, Action),
        delivery=(lib.moqt_delivery_sizeof, Delivery),
        offer=(lib.moqt_offer_sizeof, Offer),
        track=(lib.moqt_track_cfg_sizeof, TrackCfg),
    )
    for name, (fn, struct) in sizes.items():
        fn.restype = ctypes.c_size_t
        if fn() != ctypes.sizeof(struct):
            raise RuntimeError(f"ctypes layout of {name} differs from the C struct")
    lib.moqt_core_sizeof.restype = ctypes.c_size_t
    v, u, z, i64, u64 = ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_int64, ctypes.c_uint64
    signatures = {
        "moqt_core_init": (None, [v, ctypes.POINTER(TrackCfg), u, ctypes.c_bool]),
        "moqt_core_setup": (z, [v, v, z, ctypes.c_char_p, ctypes.c_char_p]),
        "moqt_core_rx": (None, [v, i64, ctypes.c_char_p, z, ctypes.c_bool, u64]),
        "moqt_core_stream_cancelled": (None, [v, i64]),
        "moqt_core_next_action": (ctypes.c_bool, [v, ctypes.POINTER(Action)]),
        "moqt_core_publish_track": (ctypes.c_int, [v, u, ctypes.c_bool, ctypes.c_bool]),
        "moqt_core_publish_namespace": (ctypes.c_int, [v]),
        "moqt_core_bidi_opened": (None, [v, u, i64]),
        "moqt_core_goaway": (ctypes.c_int, [v, v, z, u64]),
        "moqt_core_publish": (ctypes.c_int, [v, u, ctypes.POINTER(Offer), u64, ctypes.POINTER(Delivery), u]),
        "moqt_core_stream_opened": (None, [v, u]),
        "moqt_core_cache_put": (
            ctypes.c_int,
            [v, u, ctypes.POINTER(Offer), v, u64, ctypes.POINTER(ctypes.c_void_p)],
        ),
        "moqt_core_cache_evict": (v, [v, u]),
        "moqt_core_cache_count": (u, [v, u]),
        "moqt_core_cache_expire": (v, [v, u64]),
        "moqt_core_fetch_next": (
            ctypes.c_int,
            [v, u, v, z, ctypes.POINTER(z), ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(z)],
        ),
        "moqt_core_fetch_closed": (None, [v, u]),
        "moqt_core_sub_end": (None, [v, u, u64]),
        "moqt_core_sub_finish": (None, [v, u, ctypes.c_char_p, z]),
        "moqt_core_track_end": (None, [v, u]),
        "moqt_core_active_subs": (u, [v, u]),
        "moqt_core_any_forwarding": (ctypes.c_bool, [v, u]),
        "moqt_core_sub_streams": (u64, [v, u]),
        "moqt_core_sub_state": (ctypes.c_int, [v, u]),
        "moqt_core_fetch_sub": (ctypes.c_int, [v, u]),
        "moqt_core_fetch_priority": (ctypes.c_int, [v, u]),
        "moqt_core_fetch_descending": (ctypes.c_int, [v, u]),
        "moqt_core_peer_max_ranges": (u64, [v]),
        "moqt_core_is_closed": (ctypes.c_int, [v]),
        "moqt_core_set_lifetime": (None, [v, u64]),
        "moqt_core_host_extensions": (None, [v, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]),
        "moqt_core_source_expired": (u64, [v]),
        "moqt_core_host_session_reset": (None, [v]),
        "moqt_core_tick": (None, [v, u64]),
        "moqt_core_state_notify": (ctypes.c_int, [v, u]),
    }
    for name, (restype, argtypes) in signatures.items():
        fn = getattr(lib, name)
        fn.restype = restype
        fn.argtypes = argtypes
    _LIB = lib
    return lib


DEFAULT_TRACKS = [
    dict(ns=(b"aiot", b"esp32"), name=b"preview/jpeg", priority=192, group_order=2, object_timeout_ms=2000),
    dict(ns=(b"aiot", b"esp32"), name=b"ai/events", priority=64),
    dict(ns=(b"aiot", b"esp32"), name=b"evidence/jpeg", priority=128, object_timeout_ms=10000),
    dict(ns=(b"aiot", b"status"), name=b"health", dynamic_groups=True),
]


def _read_vi(b, p):
    n, mask = 1, 128
    while mask and b[p] & mask:
        n += 1
        mask >>= 1
    v = 0 if n == 9 else b[p] & ((1 << (8 - n)) - 1)
    for x in b[p + 1 : p + n]:
        v = (v << 8) | x
    return v, p + n


def _vi(v):
    n = next((n for n in range(1, 9) if v < 2 ** (7 * n)), 9)
    if n == 9:
        return b"\xff" + v.to_bytes(8, "big")
    return bytes([((0xFF << (9 - n)) & 255) | (v >> (8 * (n - 1)))]) + v.to_bytes(n, "big")[1:]


def _kvp(pairs):
    out, prev = b"", 0
    for key, value in pairs:
        out += _vi(key - prev) + _vi(value)
        prev = key
    return out


def now_ms():
    return int(time.monotonic() * 1000)


class CorePublisher(QuicConnectionProtocol):
    """QUIC client and Original Publisher."""

    def __init__(self, *a, tracks=None, authority=b"localhost:14568", extensions=None, state=None, **kw):
        super().__init__(*a, **kw)
        self.lib = library()
        # `state` carries the core and the retained payloads over to the next session.
        self.state = state if state is not None else {}
        resumed = "core" in self.state
        self.core = self.state.setdefault("core", ctypes.create_string_buffer(self.lib.moqt_core_sizeof()))
        self.track_cfg = tracks or DEFAULT_TRACKS
        cfg = (TrackCfg * len(self.track_cfg))()
        self._strings = []
        for slot, t in zip(cfg, self.track_cfg):
            for i, part in enumerate(t["ns"]):
                slot.ns[i] = part
            slot.ns_count = len(t["ns"])
            slot.name = t["name"]
            for key in (
                "priority",
                "group_order",
                "object_timeout_ms",
                "subgroup_timeout_ms",
                "max_cache_ms",
                "source_timeout_ms",
            ):
                setattr(slot, key, t.get(key, 0))
            slot.dynamic_groups = t.get("dynamic_groups", False)
        self._cfg = self.state.setdefault("cfg", cfg)  # the core keeps pointers into it
        if resumed:
            self.lib.moqt_core_host_session_reset(self.core)
        else:
            self.lib.moqt_core_init(self.core, self._cfg, len(self.track_cfg), True)
        if extensions and not resumed:
            # message, subscriptions, fetches: -1 leaves the limit unstated
            self.lib.moqt_core_host_extensions(
                self.core,
                extensions.get("message", -1),
                extensions.get("subscriptions", -1),
                extensions.get("fetches", -1),
                1 if extensions.get("source_timeout") else 0,
            )
        self.authority = authority
        self.control = None
        self.events = []
        self.payloads = self.state.setdefault("payloads", {})  # handle -> bytes retained for FETCH
        self.next_handle = max(self.payloads, default=0) + 1
        self.data_streams = {}  # stream -> subscription index
        self.fetch_streams = {}  # fetch index -> stream
        self.closed_code = None
        self.peer_closed = None
        self.ready = asyncio.Event()
        self.changed = asyncio.Event()
        self.hold = {}  # track -> seconds to wait before the object reaches the transport
        self.datagram_limit = 1100
        self.fetch_delay = 0.0
        self.last_group = self.state.setdefault("last_group", {})
        self.delayed = set()

    # -- plumbing ---------------------------------------------------------
    def note(self, kind, **kw):
        self.events.append(dict(kind=kind, **kw))
        self.changed.set()

    def quic_event_received(self, e):
        if isinstance(e, HandshakeCompleted):
            b = ctypes.create_string_buffer(256)
            n = self.lib.moqt_core_setup(self.core, b, 256, b"/", self.authority)
            self.control = self._quic.get_next_available_stream_id(is_unidirectional=True)
            self._quic.send_stream_data(self.control, b.raw[:n])
            self.ready.set()
        elif isinstance(e, StreamDataReceived):
            self.lib.moqt_core_rx(self.core, e.stream_id, e.data, len(e.data), e.end_stream, now_ms())
            self.drain()
        elif isinstance(e, (StreamReset, StopSendingReceived)):
            self.on_cancel(e.stream_id, e.error_code, isinstance(e, StopSendingReceived))
        elif isinstance(e, ConnectionTerminated):
            self.peer_closed = e.error_code
            self.note("terminated", code=e.error_code)

    def on_cancel(self, stream, code, stop):
        if stream in self.data_streams:
            # Section 11.3.2: the publisher answers STOP_SENDING with a reset.
            if stop:
                self._quic.reset_stream(stream, RST_CANCELLED)
            self.note("data_cancelled", stream=stream, sub=self.data_streams.pop(stream), code=code)
            return
        for index, s in list(self.fetch_streams.items()):
            if s == stream:
                if stop:
                    self._quic.reset_stream(stream, RST_CANCELLED)
                self.fetch_streams.pop(index)
                self.lib.moqt_core_fetch_closed(self.core, index)
                self.note("fetch_cancelled", index=index)
                self.drain()
                return
        self.lib.moqt_core_stream_cancelled(self.core, stream)
        self.drain()

    def drain(self):
        a = Action()
        while self.lib.moqt_core_next_action(self.core, ctypes.byref(a)):
            data = ctypes.string_at(a.data, a.len) if a.len else b""
            self.handle(a.kind, a.stream, a.code, a.index, a.track, a.token, data, a.fin)
        self.transmit()

    def handle(self, kind, stream, code, index, track, token, data, fin):
        q = self._quic
        if kind == ACT_SEND:
            q.send_stream_data(stream, data, end_stream=fin)
        elif kind == ACT_OPEN_BIDI:
            sid = q.get_next_available_stream_id()
            q.send_stream_data(sid, data)
            self.lib.moqt_core_bidi_opened(self.core, token, sid)
            self.note("opened", stream=sid, token=token, track=track)
        elif kind == ACT_FETCH_START:
            self.serve_fetch(index)
        elif kind == ACT_FETCH_CANCEL:
            sid = self.fetch_streams.pop(index, None)
            if sid is not None:
                q.reset_stream(sid, code)
            self.note("fetch_cancel", index=index)
        elif kind == ACT_FILL_FAILED:
            sid = q.get_next_available_stream_id(is_unidirectional=True)
            q.send_stream_data(sid, data)
            q.reset_stream(sid, code)
            self.note("fill_failed", sub=index)
        elif kind == ACT_SUB_ENDING:
            self.note("sub_ending", sub=index, status=code)
            self.lib.moqt_core_sub_finish(self.core, index, b"", 0)
        elif kind == ACT_SUB_CANCELLED:
            for sid, sub in list(self.data_streams.items()):
                if sub == index:
                    q.reset_stream(sid, RST_CANCELLED)
                    self.data_streams.pop(sid)
            try:
                q.reset_stream(stream, RST_CANCELLED)
            except Exception:
                pass
            self.note("sub_cancelled", sub=index, code=code)
        elif kind == ACT_RESET_STREAM:
            try:
                q.reset_stream(stream, code)
            except Exception:
                pass
            self.note("reset", stream=stream, code=code)
        elif kind == ACT_STOP_SENDING:
            q.stop_stream(stream, code)
            self.note("stop_sending", stream=stream, code=code)
        elif kind == ACT_CLOSE_SESSION:
            self.closed_code = code
            self.note("close", code=code)
            self.close(error_code=code, reason_phrase="moqt")
        elif kind == ACT_SUBSCRIBED:
            self.note("subscribed", sub=index, track=track, stream=stream)
        elif kind == ACT_UPDATED:
            self.note("updated", sub=index, track=track)
        elif kind == ACT_NEW_GROUP:
            self.note("new_group", track=track, value=code)
        elif kind == ACT_GOAWAY:
            self.note("goaway", timeout=code, uri=data, stream=stream, request=index)
        elif kind == ACT_PEER_FIN:
            self.note("peer_fin", sub=index)
        elif kind == ACT_NAMESPACE_RESULT:
            self.note("namespace_result", code=code)
        elif kind == ACT_REQUEST_CLOSED:
            self.note("request_closed", stream=stream)

    def serve_fetch(self, index):
        if self.fetch_delay and index not in self.delayed:
            # A constrained device needs time to send a retained object; the fetch keeps its place.
            self.delayed.add(index)
            asyncio.get_running_loop().call_later(self.fetch_delay, self.serve_fetch, index)
            return
        self.delayed.discard(index)
        q = self._quic
        sid = q.get_next_available_stream_id(is_unidirectional=True)
        self.fetch_streams[index] = sid
        sub = self.lib.moqt_core_fetch_sub(self.core, index)
        head = ctypes.create_string_buffer(256)
        hl, pl, handle = ctypes.c_size_t(), ctypes.c_size_t(), ctypes.c_void_p()
        count = 0
        while True:
            rv = self.lib.moqt_core_fetch_next(
                self.core, index, head, 256, ctypes.byref(hl), ctypes.byref(handle), ctypes.byref(pl)
            )
            if rv != 1:
                break
            q.send_stream_data(sid, head.raw[: hl.value])
            if handle.value:
                payload = self.payloads[handle.value]
                assert len(payload) == pl.value
                q.send_stream_data(sid, payload)
                count += 1
        q.send_stream_data(sid, b"", end_stream=True)
        self.fetch_streams.pop(index, None)
        self.lib.moqt_core_fetch_closed(self.core, index)
        self.note("fetch_served", index=index, objects=count, stream=sid, fill_for=sub)
        self.drain()

    # -- application API ---------------------------------------------------
    def publish(
        self,
        track,
        group,
        payload=b"",
        object_id=0,
        subgroup=0,
        props=b"",
        priority=None,
        datagram=False,
        status=0,
        end_of_group=True,
        retain=True,
        source_age_ms=None,
        announce_gap=False,
        deliver=True,
        send=True,
    ):
        """Offers one object. Returns the list of subscriptions it was delivered to."""
        if announce_gap:
            # PRIOR_GROUP_ID_GAP, as the firmware adds it for mission results.
            last = self.last_group.get(track)
            gap = group if last is None else group - last - 1
            pairs = [(0x3C, gap)]
            p = 0
            prev = 0
            while p < len(props):
                delta, p = _read_vi(props, p)
                key = prev + delta
                prev = key
                value, p = _read_vi(props, p)
                pairs.append((key, value))
            props = _kvp(pairs)
            self.last_group[track] = group
        offer = Offer(
            group=group,
            object=object_id,
            subgroup=subgroup,
            priority=priority or 0,
            status=status,
            has_priority=priority is not None,
            end_of_group=end_of_group,
            datagram=datagram,
            props=props or None,
            props_len=len(props),
            payload_len=len(payload),
            has_source_age=source_age_ms is not None,
            source_age_ms=source_age_ms or 0,
        )
        out = (Delivery * 6)()
        # deliver=False models a device whose stream slot is busy: the object is only retained.
        n = self.lib.moqt_core_publish(self.core, track, ctypes.byref(offer), now_ms(), out, 6 if deliver else 0)
        if n < 0:
            raise ValueError("the core refused the object")
        if retain and status == 0:
            handle = self.next_handle
            self.next_handle += 1
            evicted = ctypes.c_void_p()
            if self.lib.moqt_core_cache_put(
                self.core, track, ctypes.byref(offer), handle, now_ms(), ctypes.byref(evicted)
            ) == 0:
                self.payloads[handle] = bytes(payload)
                if evicted.value:
                    self.payloads.pop(evicted.value, None)
        delivered = []
        for d in out[:n]:
            head = bytes(d.head[: d.head_len])
            delivered.append(
                dict(
                    head=head,
                    sub=d.sub,
                    subscriber_priority=d.subscriber_priority,
                    publisher_priority=d.publisher_priority,
                    group_order=d.group_order,
                    object_timeout_ms=d.object_timeout_ms,
                    subgroup_timeout_ms=d.subgroup_timeout_ms,
                    source_timeout_ms=d.source_timeout_ms,
                    datagram=d.datagram,
                )
            )
            if not send:
                continue  # the caller owns the stream and its pacing
            if d.datagram:
                if len(head) + len(payload) > self.datagram_limit:
                    self.note("datagram_dropped", group=group)  # section 11.2: dropped without notice
                    continue
                self._quic.send_datagram_frame(head + bytes(payload))
                continue
            sid = self._quic.get_next_available_stream_id(is_unidirectional=True)
            self.lib.moqt_core_stream_opened(self.core, d.sub)
            hold = self.hold.get(track, 0)
            late = d.source_timeout_ms and d.source_age_ms + hold * 1000 >= d.source_timeout_ms
            if late or (d.object_timeout_ms and hold * 1000 > d.object_timeout_ms):
                # Section 5.2: the object expired before it reached the transport.
                # The header goes out first so that the subscriber can attribute the reset
                # (section 11.3.2 recommends RESET_STREAM_AT for the same purpose).
                self._quic.send_stream_data(sid, head[:3])
                self.transmit()
                asyncio.get_running_loop().call_later(0.05, self.expire, sid)
                self.note("timeout_reset", stream=sid, group=group, sub=d.sub)
                continue
            self.data_streams[sid] = d.sub
            self._quic.send_stream_data(sid, head + bytes(payload), end_stream=True)
            self.data_streams.pop(sid, None)
        self.drain()
        return delivered

    def expire(self, sid):
        self._quic.reset_stream(sid, RST_DELIVERY_TIMEOUT)
        self.transmit()

    def set_lifetime(self, ms):
        self.lib.moqt_core_set_lifetime(self.core, ms)

    def tick(self):
        self.lib.moqt_core_tick(self.core, now_ms())
        self.drain()

    def state_notify(self, sub):
        rv = self.lib.moqt_core_state_notify(self.core, sub)
        self.drain()
        return rv

    def end_track(self, track):
        self.lib.moqt_core_track_end(self.core, track)
        self.drain()

    def end_subscription(self, sub, status):
        self.lib.moqt_core_sub_end(self.core, sub, status)
        self.drain()

    def publish_track(self, track, forward=True, early=False):
        rv = self.lib.moqt_core_publish_track(self.core, track, forward, early)
        self.drain()
        return rv

    def publish_namespace(self):
        rv = self.lib.moqt_core_publish_namespace(self.core)
        self.drain()
        return rv

    def send_goaway(self, timeout_ms=0):
        b = ctypes.create_string_buffer(64)
        n = self.lib.moqt_core_goaway(self.core, b, 64, timeout_ms)
        self._quic.send_stream_data(self.control, b.raw[:n])
        self.transmit()

    def source_expired(self):
        return self.lib.moqt_core_source_expired(self.core)

    def active(self, track):
        return self.lib.moqt_core_active_subs(self.core, track)

    def streams_of(self, sub):
        return self.lib.moqt_core_sub_streams(self.core, sub)

    def seen(self, kind):
        return [e for e in self.events if e["kind"] == kind]

    async def until(self, predicate, timeout=5.0):
        deadline = asyncio.get_running_loop().time() + timeout
        while not predicate():
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                raise TimeoutError("publisher condition not reached")
            self.changed.clear()
            try:
                await asyncio.wait_for(self.changed.wait(), min(remaining, 0.05))
            except asyncio.TimeoutError:
                pass
