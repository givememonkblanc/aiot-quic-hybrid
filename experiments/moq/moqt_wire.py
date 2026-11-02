"""MOQT draft-21 wire codec for the subscriber side.

Written independently of the C codec in src/components/transport so that the two
implementations check each other. Section numbers refer to
draft-ietf-moq-transport-21.
"""

from dataclasses import dataclass

# Message types (section 9, table 5).
REQUEST_UPDATE, SUBSCRIBE, SUBSCRIBE_OK, REQUEST_ERROR = 0x2, 0x3, 0x4, 0x5
PUBLISH_NAMESPACE, REQUEST_OK, NAMESPACE, PUBLISH_DONE = 0x6, 0x7, 0x8, 0xB
TRACK_STATUS, NAMESPACE_DONE, PUBLISH_SKIPPED, GOAWAY = 0xD, 0xE, 0xF, 0x10
FETCH, FETCH_OK, PUBLISH, PUBLISH_STATE_NOTIFY = 0x16, 0x18, 0x1D, 0x22
SUBSCRIBE_NAMESPACE, SUBSCRIBE_TRACKS, SETUP = 0x50, 0x51, 0x2F00
FETCH_HEADER, PADDING_STREAM, PADDING_DATAGRAM = 0x05, 0x132B3E28, 0x132B3E29

# Message parameters (section 16.7) and their value encodings (section 9.20).
P_OBJECT_TIMEOUT, P_TOKEN, P_RENDEZVOUS, P_SUBGROUP_TIMEOUT = 0x02, 0x03, 0x04, 0x06
P_EXPIRES, P_LARGEST, P_FILL_TIMEOUT, P_FORWARD = 0x08, 0x09, 0x0A, 0x10
P_PRIORITY, P_LOCATION, P_GROUP_ORDER, P_FILL = 0x20, 0x21, 0x22, 0x23
P_SUBGROUP_FILTER, P_OBJECTID_FILTER, P_PRIORITY_FILTER = 0x25, 0x26, 0x27
P_OBJECT_PROPERTY_FILTER, P_TRACK_PROPERTY_FILTER = 0x28, 0x29
P_NEW_GROUP, P_PREFIX, P_INCLUDE_PROPERTIES = 0x32, 0x34, 0x35
# Extensions proposed for constrained AIoT endpoints; not part of draft-21.
# See papers/mission_aiot_moq/MOQT_AIOT_EXTENSIONS_KO.md.
OPT_MAX_CONTROL_MESSAGE, OPT_MAX_SUBSCRIPTIONS, OPT_MAX_FETCHES = 0xA1E0, 0xA1E2, 0xA1E4
OPT_SOURCE_TIMEOUT = 0xA1E8
P_SOURCE_TIMEOUT = 0xA1EA
X_SOURCE_AGE, X_SOURCE_TIMEOUT = 0xA1EC, 0xA1EE
_VARINT = {
    P_OBJECT_TIMEOUT,
    P_RENDEZVOUS,
    P_SUBGROUP_TIMEOUT,
    P_EXPIRES,
    P_FILL_TIMEOUT,
    P_NEW_GROUP,
    P_SOURCE_TIMEOUT,
}
_UINT8 = {P_FORWARD, P_PRIORITY, P_GROUP_ORDER, P_INCLUDE_PROPERTIES}
_LENGTH = {
    P_TOKEN,
    P_LOCATION,
    P_FILL,
    P_SUBGROUP_FILTER,
    P_OBJECTID_FILTER,
    P_PRIORITY_FILTER,
    P_OBJECT_PROPERTY_FILTER,
    P_TRACK_PROPERTY_FILTER,
}

# Properties (section 16.8).
X_OBJECT_TIMEOUT, X_MAX_CACHE, X_SUBGROUP_TIMEOUT, X_IMMUTABLE = 0x02, 0x04, 0x06, 0x0B
X_PRIORITY, X_GROUP_ORDER, X_DYNAMIC_GROUPS = 0x0E, 0x22, 0x30
X_PRIOR_GROUP_GAP, X_PRIOR_OBJECT_GAP = 0x3C, 0x3E

ASCENDING, DESCENDING = 1, 2
STATUS_NORMAL, STATUS_END_OF_GROUP, STATUS_END_OF_TRACK = 0x0, 0x3, 0x4
PROTOCOL_VIOLATION = 0x3


class NeedMore(Exception):
    """The buffer ends inside a field."""


class Violation(ValueError):
    """The peer broke the specification; the session is closed with `code`."""

    def __init__(self, reason, code=PROTOCOL_VIOLATION):
        super().__init__(reason)
        self.code = code


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


def read_u8(b, p):
    if p >= len(b):
        raise NeedMore
    return b[p], p + 1


def read_bytes(b, p, n):
    if len(b) - p < n:
        raise NeedMore
    return bytes(b[p : p + n]), p + n


def msg(t, b):
    if len(b) > 65535:
        raise ValueError("message length")
    return vi(t) + len(b).to_bytes(2, "big") + b


def field(b):
    return vi(len(b)) + b


def read_message(b, limit=2048):
    t, p = read_vi(b)
    if len(b) - p < 2:
        raise NeedMore
    n = int.from_bytes(b[p : p + 2], "big")
    p += 2
    if n > limit:
        raise ValueError("control limit")
    if len(b) - p < n:
        raise NeedMore
    return t, bytes(b[p : p + n]), p + n


def _whole(parser, body, *args):
    """Runs a parser over a complete message body; truncation is a violation."""
    try:
        value, p = parser(body, 0, *args)
    except NeedMore:
        raise Violation("truncated message body") from None
    if p != len(body):
        raise Violation("trailing bytes in message body")
    return value


# Key-Value-Pairs (section 8.3).
def kvp(pairs):
    out = b""
    prev = 0
    for key, value in sorted(pairs, key=lambda x: x[0]):
        out += vi(key - prev)
        prev = key
        if key & 1:
            if not isinstance(value, (bytes, bytearray)) or len(value) > 65535:
                raise ValueError("odd types carry at most 65535 bytes")
            out += vi(len(value)) + bytes(value)
        else:
            out += vi(value)
    return out


def read_kvp(b):
    p = 0
    prev = 0
    out = []
    try:
        while p < len(b):
            delta, p = read_vi(b, p)
            key = prev + delta
            if key >= 2**64:
                raise Violation("type overflow")
            prev = key
            if key & 1:
                n, p = read_vi(b, p)
                if n > 65535:
                    raise Violation("value length")
                value, p = read_bytes(b, p, n)
            else:
                value, p = read_vi(b, p)
            out.append((key, value))
    except NeedMore:
        raise Violation("truncated key-value pair", 0x6) from None
    return out


def properties(b):
    """Flattens a property block; values inside IMMUTABLE_PROPERTIES are included."""
    out = {}
    for key, value in read_kvp(b):
        if key == X_IMMUTABLE:
            for inner, inner_value in read_kvp(value):
                if inner == X_IMMUTABLE:
                    raise Violation("nested immutable properties")
                out.setdefault(inner, inner_value)
        else:
            out.setdefault(key, value)
    return out


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


# Track namespace (section 8.7).
def namespace(fields):
    if len(fields) > 32:
        raise ValueError("namespace fields")
    return vi(len(fields)) + b"".join(field(f) for f in fields)


def read_namespace(b, p, allow_empty=False):
    count, p = read_vi(b, p)
    if count > 32 or (not count and not allow_empty):
        raise Violation("namespace field count")
    fields = []
    for _ in range(count):
        n, p = read_vi(b, p)
        if not n:
            raise Violation("empty namespace field")
        value, p = read_bytes(b, p, n)
        fields.append(value)
    if sum(map(len, fields)) > 4096:
        raise Violation("namespace length")
    return tuple(fields), p


# Filters (sections 8.6 and 9.20.10).
def location_filter(*fields):
    """StartGroup, StartObject, EndGroupDelta, EndObject; trailing fields may be omitted."""
    if len(fields) > 4:
        raise ValueError("location filter fields")
    return b"".join(vi(x) for x in fields)


def range_filter(ranges, set_id=0, property_type=None):
    """`ranges` is a list of (start, end) with end None for an open last range."""
    out = bytes([set_id])
    if property_type is not None:
        out += vi(property_type)
    prior = 0
    for i, (start, end) in enumerate(ranges):
        if start < prior or (end is not None and end < start):
            raise ValueError("ranges must ascend")
        out += vi(start - prior)
        if end is None:
            if i != len(ranges) - 1:
                raise ValueError("only the last range may be open")
        else:
            out += vi(end - start)
            prior = end
    return out


# Parameters (section 9.20).
def parameters(items, counted=True):
    """`items` is a list of (type, value). Filters may repeat a type."""
    out = b""
    prev = 0
    ordered = sorted(items, key=lambda x: x[0])
    for key, value in ordered:
        out += vi(key - prev)
        prev = key
        if key in _VARINT:
            out += vi(value)
        elif key in _UINT8:
            out += bytes([value])
        elif key == P_LARGEST:
            out += vi(value[0]) + vi(value[1])
        elif key == P_PREFIX:
            out += namespace(value)
        elif key in _LENGTH:
            out += field(bytes(value))
        else:
            out += bytes(value)  # deliberately unknown types, used by negative tests
    return (vi(len(ordered)) if counted else b"") + out


def read_parameters(b, p, allowed=None):
    count, p = read_vi(b, p)
    prev = 0
    out = {}
    for i in range(count):
        delta, p = read_vi(b, p)
        key = prev + delta
        prev = key
        if allowed is not None and key not in allowed:
            raise Violation(f"parameter 0x{key:x} is not allowed here")
        if key in out and not 0x25 <= key <= 0x29 and key != P_TOKEN:
            raise Violation(f"parameter 0x{key:x} repeated")
        if key in _VARINT:
            value, p = read_vi(b, p)
        elif key in _UINT8:
            value, p = read_u8(b, p)
        elif key == P_LARGEST:
            group, p = read_vi(b, p)
            obj, p = read_vi(b, p)
            value = (group, obj)
        elif key == P_PREFIX:
            value, p = read_namespace(b, p, True)
        elif key in _LENGTH:
            n, p = read_vi(b, p)
            value, p = read_bytes(b, p, n)
        else:
            raise Violation(f"unknown parameter 0x{key:x}")
        if key == P_FORWARD and value > 1:
            raise Violation("forward value")
        if key == P_GROUP_ORDER and value not in (ASCENDING, DESCENDING):
            raise Violation("group order value")
        out.setdefault(key, value)
    return out, p


# Requests sent by a subscriber.
def _track_request(kind, request_id, ns, name, params):
    return msg(kind, vi(request_id) + namespace(ns) + field(name) + parameters(params))


def subscribe(request_id, ns, name, params=()):
    return _track_request(SUBSCRIBE, request_id, ns, name, list(params))


def fetch(request_id, ns, name, params=()):
    return _track_request(FETCH, request_id, ns, name, list(params))


def track_status(request_id, ns, name, params=()):
    return _track_request(TRACK_STATUS, request_id, ns, name, list(params))


def subscribe_namespace(request_id, prefix, params=()):
    return msg(SUBSCRIBE_NAMESPACE, vi(request_id) + namespace(prefix) + parameters(list(params)))


def subscribe_tracks(request_id, prefix, params=()):
    return msg(SUBSCRIBE_TRACKS, vi(request_id) + namespace(prefix) + parameters(list(params)))


def request_update(request_id, params=()):
    return msg(REQUEST_UPDATE, vi(request_id) + parameters(list(params)))


def request_ok(params=(), props=b""):
    return msg(REQUEST_OK, parameters(list(params)) + props)


def request_error(code, retry=0, reason=b""):
    return msg(REQUEST_ERROR, vi(code) + vi(retry) + field(reason))


def goaway(uri=b"", timeout=0):
    return msg(GOAWAY, field(uri) + vi(timeout))


def setup(options=()):
    return msg(SETUP, kvp(list(options)))


# Responses and publisher-initiated messages.
_OK_PARAMETERS = {P_EXPIRES, P_LARGEST}
_PUBLISH_PARAMETERS = {
    P_OBJECT_TIMEOUT,
    P_SUBGROUP_TIMEOUT,
    P_LARGEST,
    P_FORWARD,
    P_PRIORITY,
    P_LOCATION,
    P_GROUP_ORDER,
    P_EXPIRES,
    P_SOURCE_TIMEOUT,
}


def _subscribe_ok(b, p):
    alias, p = read_vi(b, p)
    params, p = read_parameters(b, p, _OK_PARAMETERS)
    return dict(alias=alias, params=params, properties=properties(b[p:])), len(b)


def _request_ok(b, p):
    params, p = read_parameters(b, p, _OK_PARAMETERS)
    return dict(params=params, properties=properties(b[p:])), len(b)


def _reason(b, p):
    n, p = read_vi(b, p)
    if n > 1024:
        raise Violation("reason phrase length")
    return read_bytes(b, p, n)


def _request_error(b, p):
    code, p = read_vi(b, p)
    retry, p = read_vi(b, p)
    reason, p = _reason(b, p)
    out = dict(code=code, retry=retry, reason=reason.decode("utf-8", "replace"))
    if code == 0x34:
        n, p = read_vi(b, p)
        uri, p = read_bytes(b, p, n)
        ns, p = read_namespace(b, p, True)
        n, p = read_vi(b, p)
        name, p = read_bytes(b, p, n)
        out["redirect"] = dict(uri=uri, namespace=ns, name=name)
    return out, p


def _publish_done(b, p):
    status, p = read_vi(b, p)
    streams, p = read_vi(b, p)
    reason, p = _reason(b, p)
    return dict(status=status, streams=streams, reason=reason.decode("utf-8", "replace")), p


def _fetch_ok(b, p):
    end_of_track, p = read_u8(b, p)
    if end_of_track > 1:
        raise Violation("end of track value")
    group, p = read_vi(b, p)
    obj, p = read_vi(b, p)
    params, p = read_parameters(b, p, _OK_PARAMETERS)
    return (
        dict(
            end_of_track=bool(end_of_track),
            end=(group, obj),
            params=params,
            properties=properties(b[p:]),
        ),
        len(b),
    )


def _publish(b, p):
    request_id, p = read_vi(b, p)
    ns, p = read_namespace(b, p)
    n, p = read_vi(b, p)
    name, p = read_bytes(b, p, n)
    alias, p = read_vi(b, p)
    params, p = read_parameters(b, p, _PUBLISH_PARAMETERS)
    return (
        dict(
            request_id=request_id,
            namespace=ns,
            name=name,
            alias=alias,
            params=params,
            properties=properties(b[p:]),
        ),
        len(b),
    )


def _publish_namespace(b, p):
    request_id, p = read_vi(b, p)
    ns, p = read_namespace(b, p)
    params, p = read_parameters(b, p, {P_TOKEN})
    return dict(request_id=request_id, namespace=ns, params=params), p


def _suffix(b, p):
    return read_namespace(b, p, True)


def _publish_skipped(b, p):
    ns, p = read_namespace(b, p, True)
    n, p = read_vi(b, p)
    name, p = read_bytes(b, p, n)
    return dict(suffix=ns, name=name), p


def _state_notify(b, p):
    return read_parameters(b, p, {P_LARGEST, P_FORWARD, P_LOCATION})


def _goaway(b, p):
    n, p = read_vi(b, p)
    if n > 8192:
        raise Violation("new session URI length")
    uri, p = read_bytes(b, p, n)
    timeout, p = read_vi(b, p)
    return dict(uri=uri, timeout=timeout), p


_PARSERS = {
    SUBSCRIBE_OK: _subscribe_ok,
    REQUEST_OK: _request_ok,
    REQUEST_ERROR: _request_error,
    PUBLISH_DONE: _publish_done,
    FETCH_OK: _fetch_ok,
    PUBLISH: _publish,
    PUBLISH_NAMESPACE: _publish_namespace,
    NAMESPACE: _suffix,
    NAMESPACE_DONE: _suffix,
    PUBLISH_SKIPPED: _publish_skipped,
    PUBLISH_STATE_NOTIFY: _state_notify,
    GOAWAY: _goaway,
}


def parse(kind, body):
    if kind not in _PARSERS:
        raise Violation(f"unknown message type 0x{kind:x}")
    return _whole(_PARSERS[kind], body)


# Data plane (section 11).
@dataclass
class Object:
    alias: int = None
    group: int = 0
    object: int = 0
    subgroup: int = None  # None for the Datagram forwarding preference
    priority: int = None  # None: the default publisher priority applies
    status: int = STATUS_NORMAL
    end_of_group: bool = False
    datagram: bool = False
    properties: dict = None
    payload: bytes = b""

    def __post_init__(self):
        if self.properties is None:
            self.properties = {}


def is_subgroup_type(t):
    return t < 128 and bool(t & 0x10)


@dataclass
class SubgroupHeader:
    flags: int
    alias: int
    group: int
    subgroup: int  # None until the first object when the mode is "first Object ID"
    priority: int
    size: int

    @property
    def with_properties(self):
        return bool(self.flags & 0x01)

    @property
    def end_of_group(self):
        return bool(self.flags & 0x08)

    @property
    def first_object(self):
        return bool(self.flags & 0x40)


def read_subgroup_header(b):
    flags, p = read_vi(b)
    if not is_subgroup_type(flags):
        raise Violation("not a subgroup header")
    mode = (flags & 0x06) >> 1
    if mode == 3:
        raise Violation("reserved subgroup ID mode")
    alias, p = read_vi(b, p)
    group, p = read_vi(b, p)
    subgroup = 0 if mode == 0 else None
    if mode == 2:
        subgroup, p = read_vi(b, p)
    priority = None
    if not flags & 0x20:
        priority, p = read_u8(b, p)
    return SubgroupHeader(flags, alias, group, subgroup, priority, p)


def read_subgroup_object(b, p, header, previous):
    """Returns (Object, next position). `previous` is the prior Object ID or None."""
    delta, p = read_vi(b, p)
    object_id = delta if previous is None else previous + delta + 1
    if object_id >= 2**64:
        raise Violation("object ID overflow")
    props = {}
    if header.with_properties:
        n, p = read_vi(b, p)
        raw, p = read_bytes(b, p, n)
        props = properties(raw)
    size, p = read_vi(b, p)
    status = STATUS_NORMAL
    payload = b""
    if size:
        payload, p = read_bytes(b, p, size)
    else:
        status, p = read_vi(b, p)
        if status not in (STATUS_NORMAL, STATUS_END_OF_GROUP, STATUS_END_OF_TRACK):
            raise Violation("object status")
        if status != STATUS_NORMAL and props:
            raise Violation("properties on a status object")
    subgroup = header.subgroup
    if subgroup is None:
        subgroup = header.subgroup = object_id
    return (
        Object(
            alias=header.alias,
            group=header.group,
            object=object_id,
            subgroup=subgroup,
            priority=header.priority,
            status=status,
            properties=props,
            payload=payload,
        ),
        p,
    )


def read_datagram(b):
    try:
        flags, p = read_vi(b)
        if flags in (PADDING_DATAGRAM,):
            return None
        if flags >= 128 or flags & 0x10 or flags & ~0x3F or (flags & 0x20 and flags & 0x02):
            raise Violation("datagram type flags")
        alias, p = read_vi(b, p)
        group, p = read_vi(b, p)
        object_id = 0
        if not flags & 0x04:
            object_id, p = read_vi(b, p)
        priority = None
        if not flags & 0x08:
            priority, p = read_u8(b, p)
        props = {}
        if flags & 0x01:
            n, p = read_vi(b, p)
            if not n:
                raise Violation("empty properties with the PROPERTIES bit")
            raw, p = read_bytes(b, p, n)
            props = properties(raw)
        status = STATUS_NORMAL
        payload = b""
        if flags & 0x20:
            status, p = read_vi(b, p)
            if p != len(b):
                raise Violation("payload after object status")
            if status != STATUS_NORMAL and props:
                raise Violation("properties on a status object")
        else:
            payload = bytes(b[p:])
    except NeedMore:
        raise Violation("truncated datagram") from None
    return Object(
        alias=alias,
        group=group,
        object=object_id,
        subgroup=None,
        priority=priority,
        status=status,
        end_of_group=bool(flags & 0x02),
        datagram=True,
        properties=props,
        payload=payload,
    )


END_NONEXISTENT, END_UNKNOWN, END_TIMED_OUT = 0x8C, 0x10C, 0x20C


class FetchReader:
    """Incremental decoder for a fetch stream (section 11.4)."""

    def __init__(self, descending=False):
        self.descending = descending
        self.request_id = None
        self.prev = None  # (group, object)
        self.subgroup = None
        self.priority = None
        self.ranges = []

    def header(self, b):
        t, p = read_vi(b)
        if t != FETCH_HEADER:
            raise Violation("not a fetch stream")
        self.request_id, p = read_vi(b, p)
        return p

    def next(self, b, p):
        """Returns (Object or None for an End of Range marker, next position)."""
        flags, p = read_vi(b, p)
        if flags in (END_NONEXISTENT, END_UNKNOWN, END_TIMED_OUT):
            group, p = read_vi(b, p)
            obj, p = read_vi(b, p)
            self.ranges.append((flags, group, obj))
            self.prev = (group, obj)
            return None, p
        if flags >= 128:
            raise Violation("fetch serialization flags")
        first = self.prev is None
        if first and (not flags & 0x08 or not flags & 0x04):
            raise Violation("the first fetch object carries absolute IDs")
        group = None
        if flags & 0x08:
            delta, p = read_vi(b, p)
            if first:
                group = delta
            else:
                group = self.prev[0] - delta - 1 if self.descending else self.prev[0] + delta + 1
            if not 0 <= group < 2**64:
                raise Violation("group ID range")
        else:
            group = self.prev[0]
        datagram = bool(flags & 0x40)
        subgroup = None
        if not datagram:
            mode = flags & 0x03
            if mode == 0:
                subgroup = 0
            elif mode == 3:
                subgroup, p = read_vi(b, p)
            else:
                if self.subgroup is None:
                    raise Violation("no prior subgroup ID")
                subgroup = self.subgroup + (mode == 2)
        if flags & 0x04:
            delta, p = read_vi(b, p)
            obj = delta if flags & 0x08 else self.prev[1] + delta
        else:
            obj = self.prev[1] + 1
        if flags & 0x10:
            priority, p = read_u8(b, p)
        else:
            if self.priority is None:
                raise Violation("no prior priority")
            priority = self.priority
        props = {}
        if flags & 0x20:
            n, p = read_vi(b, p)
            raw, p = read_bytes(b, p, n)
            props = properties(raw)
        size, p = read_vi(b, p)
        payload, p = read_bytes(b, p, size)
        self.prev = (group, obj)
        self.priority = priority
        if not datagram:
            self.subgroup = subgroup
        return (
            Object(
                group=group,
                object=obj,
                subgroup=subgroup,
                priority=priority,
                datagram=datagram,
                properties=props,
                payload=payload,
            ),
            p,
        )
