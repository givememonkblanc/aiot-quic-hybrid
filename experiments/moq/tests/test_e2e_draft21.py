"""End-to-end draft-21 checks over real QUIC on localhost.

Publisher: the portable C core that ships in the firmware, driven through ctypes
(host_core.py). Subscriber: the independent Python implementation (subscriber.py).
Test names carry the checklist IDs of MOQT_DRAFT21_CHECKLIST_KO.md.
"""

import asyncio
import datetime
import socket
import tempfile
import unittest
from pathlib import Path

from aioquic.asyncio import connect, serve
from aioquic.quic.configuration import QuicConfiguration
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

import moqt_wire as w
from host_core import CorePublisher
from subscriber import ALPN, LimitReached, RequestFailed, Subscriber

NS = (b"aiot", b"esp32")
STATUS_NS = (b"aiot", b"status")
PREVIEW, EVENTS, EVIDENCE, HEALTH = 0, 1, 2, 3
DETECTIONS = 0x7A  # application property (section 8.4): detection count
CAPTURE_MS = 0x78


def certificate(directory):
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), False)
        .sign(key, hashes.SHA256())
    )
    cert_path, key_path = directory / "cert.pem", directory / "key.pem"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    return cert_path, key_path


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Session:
    """One subscriber (server) and one publisher (client) on localhost."""

    def __init__(self, cert, key, publisher=None, tracks=None, **subscriber_options):
        self.cert, self.key = cert, key
        self.options = subscriber_options
        self.publisher = publisher  # extensions the publisher advertises
        self.tracks = tracks
        self.subscribers = []

    async def __aenter__(self):
        port = free_port()
        server_config = QuicConfiguration(
            is_client=False,
            alpn_protocols=[ALPN],
            max_datagram_frame_size=1200,
            max_data=2**22,
            max_stream_data=2**20,
            idle_timeout=20,
        )
        server_config.load_cert_chain(self.cert, self.key)

        def make(*a, **kw):
            s = Subscriber(*a, **self.options, **kw)
            self.subscribers.append(s)
            return s

        self.server = await serve("127.0.0.1", port, configuration=server_config, create_protocol=make)
        client_config = QuicConfiguration(
            is_client=True,
            alpn_protocols=[ALPN],
            max_datagram_frame_size=1200,
            server_name="localhost",
            idle_timeout=20,
        )
        client_config.load_verify_locations(str(self.cert))
        self.context = connect(
            "127.0.0.1",
            port,
            configuration=client_config,
            create_protocol=lambda *a, **kw: CorePublisher(*a, extensions=self.publisher, tracks=self.tracks, **kw),
        )
        self.pub = await self.context.__aenter__()
        await asyncio.wait_for(self.pub.ready.wait(), 5)
        for _ in range(100):
            if self.subscribers:
                break
            await asyncio.sleep(0.01)
        self.sub = self.subscribers[0]
        await asyncio.wait_for(self.sub.setup_done.wait(), 5)
        await self.pub.until(lambda: self.pub.lib.moqt_core_peer_max_ranges(self.pub.core) > 0 or not self.options.get("max_filter_ranges", 8))
        return self

    async def __aexit__(self, *exc):
        try:
            await self.context.__aexit__(None, None, None)
        finally:
            self.server.close()
        await asyncio.sleep(0)


class Draft21(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="moqt-e2e-")
        cls.cert, cls.key = certificate(Path(cls.directory.name))

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def session(self, publisher=None, tracks=None, **options):
        return Session(self.cert, self.key, publisher=publisher, tracks=tracks, **options)

    async def settle(self, s, predicate, timeout=5.0):
        await s.sub.until(predicate, timeout)

    async def test_A_session_setup_options(self):
        async with self.session(max_request_updates=3) as s:
            self.assertEqual(s.sub.peer_setup[1], b"/")  # A3 PATH
            self.assertEqual(s.sub.peer_setup[5], b"localhost:14568")  # A3 AUTHORITY
            self.assertEqual(s.sub.peer_setup[7], b"aiot-moqt21/2")  # A4
            self.assertEqual(s.sub.peer_setup[6], 8)  # A5 MAX_FILTER_RANGES
            self.assertEqual(s.sub.peer_setup[8], 4)  # A6 MAX_REQUEST_UPDATES
            self.assertEqual(s.pub.lib.moqt_core_peer_max_ranges(s.pub.core), 8)
            self.assertFalse(s.sub.errors)

    async def test_C_subscribe_deliver_done(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(NS, b"ai/events")
            self.assertEqual(sub.alias, 2)  # C2
            self.assertNotIn(w.P_LARGEST, sub.ok["params"])  # nothing published yet
            self.assertEqual(sub.ok["properties"][w.X_PRIORITY], 64)  # B12
            props = w.kvp([(CAPTURE_MS, 123456), (DETECTIONS, 3)])
            delivered = s.pub.publish(EVENTS, 42, b'{"n":3}', props=props, priority=32)
            self.assertEqual(len(delivered), 1)
            self.assertEqual(delivered[0]["publisher_priority"], 32)  # F2
            s.pub.publish(EVENTS, 43, b'{"n":0}')
            await self.settle(s, lambda: len(sub.objects) == 2)
            first, second = sub.objects
            self.assertEqual((first.group, first.object, first.subgroup), (42, 0, 0))  # B2, B3
            self.assertEqual(first.priority, 32)
            self.assertEqual(first.properties, {CAPTURE_MS: 123456, DETECTIONS: 3})  # B6, B7
            self.assertEqual(first.payload, b'{"n":3}')
            self.assertIsNone(second.priority)  # the track default applies
            s.pub.end_track(EVENTS)
            await asyncio.wait_for(sub.finished.wait(), 5)
            self.assertEqual(sub.done["status"], 0x2)  # C7 TRACK_ENDED
            self.assertEqual(sub.done["streams"], 2)
            self.assertEqual(sub.streams, 2)
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.subscribe(NS, b"ai/events")
            self.assertEqual(failure.exception.code, 0x10)  # C3 DOES_NOT_EXIST
            late = await s.sub.subscribe(NS, b"preview/jpeg")
            self.assertEqual(late.ok["properties"][w.X_GROUP_ORDER], w.DESCENDING)
            self.assertFalse(s.sub.errors)

    async def test_C_largest_object_and_include_properties(self):
        async with self.session() as s:
            s.pub.publish(EVIDENCE, 7, b"x" * 10, object_id=2)
            sub = await s.sub.subscribe(NS, b"evidence/jpeg", [(w.P_INCLUDE_PROPERTIES, 0)])
            self.assertEqual(sub.ok["params"][w.P_LARGEST], (7, 2))  # C2
            self.assertEqual(sub.ok["properties"], {})  # C12

    async def test_C_forward_update_and_multiple_subscriptions(self):
        async with self.session() as s:
            a = await s.sub.subscribe(NS, b"preview/jpeg", [(w.P_FORWARD, 0), (w.P_PRIORITY, 9)])
            b = await s.sub.subscribe(NS, b"preview/jpeg", [(w.P_GROUP_ORDER, w.ASCENDING)])  # C4
            d = s.pub.publish(PREVIEW, 1, b"j" * 100)
            self.assertEqual([x["sub"] for x in d], [1])  # C5
            self.assertEqual(d[0]["group_order"], w.ASCENDING)  # F3 subscriber value
            ok = await s.sub.update(a, [(w.P_FORWARD, 1), (w.P_PRIORITY, 2)])  # C6
            self.assertEqual(ok["params"][w.P_LARGEST], (1, 0))
            d = s.pub.publish(PREVIEW, 2, b"j" * 100)
            self.assertEqual(sorted(x["sub"] for x in d), [0, 1])
            first = next(x for x in d if x["sub"] == 0)
            self.assertEqual(first["subscriber_priority"], 2)  # F1
            self.assertEqual(first["group_order"], w.DESCENDING)  # F3 track value
            await self.settle(s, lambda: len(a.objects) == 1 and len(b.objects) == 2)
            self.assertEqual(a.objects[0].group, 2)

    async def test_C_update_failure_ends_subscription(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(NS, b"ai/events")
            bad = (w.P_OBJECT_PROPERTY_FILTER, w.range_filter([(1, None)], property_type=0x79))
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.update(sub, [bad])
            self.assertEqual(failure.exception.code, 0x36)  # INVALID_FILTER
            await asyncio.wait_for(sub.finished.wait(), 5)
            self.assertEqual(sub.done["status"], 0x8)  # UPDATE_FAILED
            self.assertEqual(s.pub.active(EVENTS), 0)

    async def test_C_cancel_subscription(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(NS, b"ai/events")
            s.sub.cancel(sub)  # C8
            await s.pub.until(lambda: s.pub.seen("sub_cancelled"))
            self.assertEqual(s.pub.active(EVENTS), 0)
            self.assertEqual(s.pub.publish(EVENTS, 1, b"x"), [])

    async def test_D_filters(self):
        async with self.session() as s:
            with_detection = await s.sub.subscribe(
                NS,
                b"ai/events",
                [(w.P_OBJECT_PROPERTY_FILTER, w.range_filter([(1, None)], property_type=DETECTIONS))],
            )  # D5
            window = await s.sub.subscribe(
                NS,
                b"preview/jpeg",
                [
                    (w.P_LOCATION, w.location_filter(5, 0, 1)),  # D1 groups 5..6
                    (w.P_SUBGROUP_FILTER, w.range_filter([(0, 0)])),  # D2
                ],
            )
            priority = await s.sub.subscribe(
                NS,
                b"evidence/jpeg",
                [
                    (w.P_PRIORITY_FILTER, w.range_filter([(0, 100)], set_id=0)),  # D4
                    (w.P_OBJECTID_FILTER, w.range_filter([(3, 5), (10, 15)], set_id=1)),  # D3, D6, D7
                ],
            )
            for group, count in [(1, 0), (2, 2), (3, 0), (4, 1)]:
                s.pub.publish(EVENTS, group, b"r", props=w.kvp([(DETECTIONS, count)]))
            s.pub.publish(EVENTS, 5, b"r")  # no property at all
            for group in range(4, 8):
                s.pub.publish(PREVIEW, group, b"low", subgroup=0, object_id=0, end_of_group=False)
                s.pub.publish(PREVIEW, group, b"high", subgroup=1, object_id=1)
            s.pub.publish(EVIDENCE, 1, b"a", priority=50)
            s.pub.publish(EVIDENCE, 2, b"b", priority=200)
            s.pub.publish(EVIDENCE, 3, b"c", priority=200, object_id=4, subgroup=4)
            s.pub.publish(EVIDENCE, 4, b"d", priority=200, object_id=12, subgroup=12)
            s.pub.publish(EVIDENCE, 5, b"e", priority=200, object_id=7, subgroup=7)
            await self.settle(
                s,
                lambda: len(with_detection.objects) == 2 and len(window.objects) == 2 and len(priority.objects) == 3,
            )
            await asyncio.sleep(0.1)
            self.assertEqual([o.group for o in with_detection.objects], [2, 4])
            self.assertEqual([(o.group, o.subgroup) for o in window.objects], [(5, 0), (6, 0)])
            self.assertEqual(sorted(o.group for o in priority.objects), [1, 3, 4])
            self.assertEqual(priority.objects[1].subgroup, 4)  # B3 subgroup ID mode 1
            with self.assertRaises(RequestFailed) as failure:  # D8
                await s.sub.subscribe(
                    NS, b"ai/events", [(w.P_OBJECTID_FILTER, w.range_filter([(i * 2, i * 2) for i in range(9)]))]
                )
            self.assertEqual(failure.exception.code, 0x36)
            self.assertFalse(s.sub.errors)

    async def test_D_filter_update_replaces_type(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(NS, b"preview/jpeg", [(w.P_SUBGROUP_FILTER, w.range_filter([(0, 0)]))])
            s.pub.publish(PREVIEW, 1, b"high", subgroup=1, object_id=1)
            await s.sub.update(sub, [(w.P_SUBGROUP_FILTER, w.range_filter([(1, 1)]))])
            s.pub.publish(PREVIEW, 2, b"high", subgroup=1, object_id=1)
            await s.sub.update(sub, [(w.P_SUBGROUP_FILTER, b"")])  # length 0 removes the filter
            s.pub.publish(PREVIEW, 3, b"low", subgroup=0)
            await self.settle(s, lambda: len(sub.objects) == 2)
            self.assertEqual([o.group for o in sub.objects], [2, 3])

    async def test_E_fetch(self):
        async with self.session() as s:
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.fetch(NS, b"evidence/jpeg")
            self.assertEqual(failure.exception.code, 0x11)  # E5 INVALID_RANGE
            payloads = {}
            for group, count in [(10, 0), (11, 2), (13, 1)]:
                payloads[group] = bytes([group]) * (1000 + group)
                s.pub.publish(EVIDENCE, group, payloads[group], props=w.kvp([(DETECTIONS, count)]))
            s.pub.publish(EVIDENCE, 13, b"second", object_id=1, subgroup=1, priority=7)
            f = await s.sub.fetch(NS, b"evidence/jpeg")  # E1-E3
            self.assertEqual(f.ok["end"], (13, 1))
            self.assertFalse(f.ok["end_of_track"])
            self.assertEqual([(o.group, o.object) for o in f.objects], [(10, 0), (11, 0), (13, 0), (13, 1)])
            self.assertEqual([o.payload for o in f.objects[:3]], [payloads[10], payloads[11], payloads[13]])
            self.assertEqual(f.objects[0].priority, 128)
            self.assertEqual(f.objects[3].priority, 7)
            self.assertEqual(f.objects[3].subgroup, 1)
            self.assertEqual(f.objects[1].properties[DETECTIONS], 2)
            f = await s.sub.fetch(NS, b"evidence/jpeg", [(w.P_GROUP_ORDER, w.DESCENDING)])  # E4
            self.assertEqual([(o.group, o.object) for o in f.objects], [(13, 0), (13, 1), (11, 0), (10, 0)])
            f = await s.sub.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(11, 0, 0))])
            self.assertEqual([o.group for o in f.objects], [11])
            self.assertEqual(f.ok["end"][0], 11)
            f = await s.sub.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(12, 0, 0))])
            self.assertEqual(f.objects, [])  # E5 header then FIN
            f = await s.sub.fetch(
                NS,
                b"evidence/jpeg",
                [(w.P_OBJECT_PROPERTY_FILTER, w.range_filter([(1, None)], property_type=DETECTIONS))],
            )  # E9
            self.assertEqual([o.group for o in f.objects], [11, 13])
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(14, 0))])
            self.assertEqual(failure.exception.code, 0x11)
            s.pub.publish(EVIDENCE, 13, b"", object_id=2, status=w.STATUS_END_OF_TRACK, retain=False)
            f = await s.sub.fetch(NS, b"evidence/jpeg")
            self.assertTrue(f.ok["end_of_track"])  # E2
            self.assertEqual(f.ok["end"], (13, 2))
            self.assertFalse(s.sub.errors)

    async def test_E_evidence_on_demand(self):
        """Results are pushed; the evidence of one observation is fetched by Group ID."""
        async with self.session() as s:
            results = await s.sub.subscribe(NS, b"ai/events")
            for group in range(20, 26):
                detections = 2 if group in (21, 24) else 0
                s.pub.publish(EVIDENCE, group, b"img%d" % group * 50)
                s.pub.publish(EVENTS, group, b"result", props=w.kvp([(DETECTIONS, detections)]))
            await self.settle(s, lambda: len(results.objects) == 6)
            wanted = [o.group for o in results.objects if o.properties.get(DETECTIONS)]
            self.assertEqual(wanted, [21, 24])
            for group in wanted:
                f = await s.sub.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(group, 0, 0))])
                self.assertEqual([(o.group, o.payload) for o in f.objects], [(group, b"img%d" % group * 50)])
            self.assertEqual(len(s.pub.seen("fetch_served")), 2)

    async def test_E_fill(self):
        async with self.session() as s:
            for group in (5, 6, 7):
                s.pub.publish(EVIDENCE, group, b"old%d" % group)
            fill = w.parameters([(w.P_LOCATION, w.location_filter(2))], counted=False)
            sub = await s.sub.subscribe(
                NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(0, 0)), (w.P_FILL, fill)]
            )  # E8
            f = sub.fills[0]
            await asyncio.wait_for(f.complete.wait(), 5)
            self.assertEqual([o.group for o in f.objects], [6, 7])
            s.pub.publish(EVIDENCE, 8, b"live")
            await self.settle(s, lambda: len(sub.objects) == 1)
            fill = w.parameters([(w.P_LOCATION, w.location_filter(5, 0, 0))], counted=False)
            await s.sub.update(sub, [(w.P_FILL, fill)])
            await asyncio.wait_for(sub.fills[1].complete.wait(), 5)
            self.assertEqual([o.group for o in sub.fills[1].objects], [5])
            s.pub.end_track(EVIDENCE)
            await asyncio.wait_for(sub.finished.wait(), 5)
            self.assertEqual(sub.done["streams"], 3)  # two fill fetch streams and one subgroup
            self.assertFalse(s.sub.errors)

    async def test_C_track_status(self):
        async with self.session() as s:
            s.pub.publish(EVENTS, 4, b"x", object_id=2, subgroup=2)
            ok = await s.sub.track_status(NS, b"ai/events")  # C11
            self.assertEqual(ok["params"][w.P_LARGEST], (4, 2))
            self.assertEqual(ok["properties"][w.X_PRIORITY], 64)
            self.assertEqual(s.pub.active(EVENTS), 0)
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.track_status(NS, b"nope")
            self.assertEqual(failure.exception.code, 0x10)
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.track_status((b".session",), b"t")  # H7
            self.assertEqual(failure.exception.code, 0x10)

    async def test_H_namespaces(self):
        async with self.session() as s:
            state = await s.sub.subscribe_namespace((b"aiot",))  # H2
            await self.settle(s, lambda: len(state["seen"]) == 2)
            self.assertEqual(state["seen"], [(b"esp32",), (b"status",)])
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.subscribe_namespace(NS)
            self.assertEqual(failure.exception.code, 0x30)  # H6 PREFIX_OVERLAP
            s.pub.end_track(HEALTH)
            await self.settle(s, lambda: state["done"] == [(b"status",)])  # H3
            self.assertEqual(state["active"], {(b"esp32",)})
            self.assertEqual(s.pub.publish_namespace(), 0)  # H1
            await s.pub.until(lambda: s.pub.seen("namespace_result"))
            self.assertEqual(s.pub.seen("namespace_result")[0]["code"], 0)
            self.assertEqual(s.sub.published_namespaces, [NS])
            self.assertFalse(s.sub.errors)

    async def test_H_subscribe_tracks_and_publish(self):
        async with self.session() as s:
            s.sub.publish_response = [(w.P_FORWARD, 1)]
            state = await s.sub.subscribe_tracks(
                NS,
                [
                    (w.P_FORWARD, 0),
                    (w.P_PRIORITY, 7),
                    (w.P_TRACK_PROPERTY_FILTER, w.range_filter([(0, 100)], property_type=w.X_PRIORITY)),  # D9
                ],
            )  # H4
            await self.settle(s, lambda: len(s.sub.seen("publish")) == 1)
            published = s.sub.seen("publish")[0]
            self.assertEqual(published["track"], "ai/events")  # the only track with priority <= 100
            await s.pub.until(lambda: s.pub.seen("subscribed"))
            d = s.pub.publish(EVENTS, 1, b"r")
            self.assertEqual(len(d), 1)
            self.assertEqual(d[0]["subscriber_priority"], 7)
            sub = s.sub.subscriptions[("publish", 0)]
            await self.settle(s, lambda: len(sub.objects) == 1)  # C9
            self.assertEqual(state["skipped"], [])
            await s.sub.update(sub, [(w.P_FORWARD, 0)])
            self.assertEqual(s.pub.publish(EVENTS, 2, b"r"), [])
            self.assertFalse(s.sub.errors)

    async def test_C_publish_initiated_and_declined(self):
        async with self.session() as s:
            self.assertEqual(s.pub.publish_track(EVIDENCE, forward=True, early=True), 0)  # C9
            await s.pub.until(lambda: s.pub.seen("subscribed"))
            s.pub.publish(EVIDENCE, 1, b"e")
            sub = s.sub.subscriptions[("publish", 0)]
            await self.settle(s, lambda: len(sub.objects) == 1)
            self.assertEqual(sub.ok["params"][w.P_FORWARD], 1)
            s.sub.publish_response = 0x20
            self.assertGreaterEqual(s.pub.publish_track(PREVIEW), 0)
            await s.pub.until(lambda: s.pub.seen("sub_cancelled"))
            self.assertEqual(s.pub.seen("sub_cancelled")[0]["code"], 0x20)  # UNINTERESTED
            self.assertEqual(s.pub.active(PREVIEW), 0)

    async def test_B_datagram(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(STATUS_NS, b"health")
            s.pub.publish(HEALTH, 1, b"heap=1", datagram=True, props=w.kvp([(CAPTURE_MS, 9)]), priority=200)  # B10
            s.pub.publish(HEALTH, 2, b"", datagram=True, status=w.STATUS_END_OF_GROUP, retain=False, end_of_group=False)
            s.pub.publish(HEALTH, 3, b"z" * 2000, datagram=True)  # exceeds the datagram size: dropped
            await self.settle(s, lambda: len(sub.objects) == 2)
            first, second = sub.objects
            self.assertTrue(first.datagram)
            self.assertEqual((first.group, first.payload, first.priority), (1, b"heap=1", 200))
            self.assertEqual(first.properties, {CAPTURE_MS: 9})
            self.assertEqual(second.status, w.STATUS_END_OF_GROUP)
            self.assertEqual(len(s.pub.seen("datagram_dropped")), 1)
            self.assertEqual(s.pub.streams_of(0), 0)  # datagrams open no streams
            f = await s.sub.fetch(STATUS_NS, b"health")
            self.assertEqual([(o.group, o.datagram) for o in f.objects], [(1, True), (3, True)])

    async def test_G_delivery_timeout(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(NS, b"preview/jpeg", [(w.P_OBJECT_TIMEOUT, 500)])
            d = s.pub.publish(PREVIEW, 1, b"j")
            self.assertEqual(d[0]["object_timeout_ms"], 500)  # G3 the smaller value
            loose = await s.sub.subscribe(NS, b"preview/jpeg", [(w.P_OBJECT_TIMEOUT, 9000)])
            d = s.pub.publish(PREVIEW, 2, b"j", props=w.kvp([(w.X_OBJECT_TIMEOUT, 100)]))
            self.assertEqual({x["object_timeout_ms"] for x in d}, {100})  # G4 property override
            d = s.pub.publish(PREVIEW, 3, b"j")
            self.assertEqual(sorted(x["object_timeout_ms"] for x in d), [500, 2000])
            await self.settle(s, lambda: len(sub.objects) == 3 and len(loose.objects) == 2)
            s.pub.hold[PREVIEW] = 1.0  # the object waits 1 s before the transport accepts it
            s.pub.publish(PREVIEW, 4, b"j")
            await self.settle(s, lambda: len(sub.resets) == 1)  # G1
            self.assertEqual(sub.resets[0]["code"], 0x2)  # DELIVERY_TIMEOUT
            # The other subscription allows 2000 ms, so exactly one copy of group 4 arrives.
            # Both subscriptions share Track Alias 1; the copy is attributed to either one.
            await self.settle(s, lambda: sum(o.group == 4 for o in sub.objects + loose.objects) == 1)
            self.assertEqual(len(s.pub.seen("timeout_reset")), 1)
            s.pub.end_track(PREVIEW)
            await asyncio.wait_for(sub.finished.wait(), 5)
            self.assertEqual(sub.done["streams"], 4)  # reset streams are counted (C7)

    async def test_A_goaway(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(NS, b"ai/events")
            s.sub.send_goaway(timeout=250)  # A14
            await s.pub.until(lambda: s.pub.seen("goaway"))
            self.assertEqual(s.pub.seen("goaway")[0]["timeout"], 250)
            self.assertEqual(s.pub.active(EVENTS), 1)
            with self.assertRaises(RequestFailed) as failure:
                await s.sub.subscribe(NS, b"preview/jpeg")
            self.assertEqual(failure.exception.code, 0x6)  # GOING_AWAY
            s.pub.send_goaway(100)
            await self.settle(s, lambda: len(s.sub.goaways) == 1)
            self.assertEqual(s.sub.goaways[0]["timeout"], 100)
            s.pub.publish(EVENTS, 1, b"still delivered")
            await self.settle(s, lambda: len(sub.objects) == 1)
            s.sub.send_goaway()
            await self.settle(s, lambda: s.sub.terminated is not None)
            self.assertEqual(s.sub.terminated["code"], 0x3)  # a second GOAWAY

    async def violation(self, action, code):
        async with self.session() as s:
            await action(s)
            await self.settle(s, lambda: s.sub.terminated is not None)
            self.assertEqual(s.sub.terminated["code"], code)
            self.assertEqual(s.pub.closed_code, code)

    async def test_A_violations(self):
        async def unknown_parameter(s):  # C15
            s.sub.send_raw(s.sub.new_request_stream(), w.subscribe(1, NS, b"ai/events", [(0x01, b"\x00")]))

        async def disallowed_parameter(s):
            s.sub.send_raw(s.sub.new_request_stream(), w.fetch(1, NS, b"ai/events", [(w.P_FORWARD, 1)]))

        async def wrong_parity(s):  # A11
            s.sub.send_raw(s.sub.new_request_stream(), w.subscribe(2, NS, b"ai/events"))

        async def duplicate_id(s):
            await s.sub.subscribe(NS, b"ai/events", request_id=1)
            s.sub.send_raw(s.sub.new_request_stream(), w.subscribe(1, NS, b"preview/jpeg"))

        async def first_message(s):  # A12
            s.sub.send_raw(s.sub.new_request_stream(), w.request_update(1))

        async def unknown_message(s):  # A10
            s.sub.send_raw(s.sub.new_request_stream(), w.msg(0x3F, b""))

        async def length_mismatch(s):  # A9
            body = w.vi(1) + w.namespace(NS) + w.field(b"ai/events") + w.vi(0) + b"\x00"
            s.sub.send_raw(s.sub.new_request_stream(), w.msg(w.SUBSCRIBE, body))

        async def too_many_updates(s):  # A6
            sub = await s.sub.subscribe(NS, b"ai/events")
            burst = b"".join(w.request_update(101 + 2 * i) for i in range(5))
            s.sub.send_raw(sub.stream, burst)

        async def register_token(s):  # A15
            token = w.vi(1) + w.vi(1) + w.vi(0) + b"secret"
            s.sub.send_raw(s.sub.new_request_stream(), w.subscribe(1, NS, b"ai/events", [(w.P_TOKEN, token)]))

        for action, code in [
            (unknown_parameter, 0x3),
            (disallowed_parameter, 0x3),
            (wrong_parity, 0x4),
            (duplicate_id, 0x4),
            (first_message, 0x3),
            (unknown_message, 0x3),
            (length_mismatch, 0x3),
            (too_many_updates, 0x1B),
            (register_token, 0x13),
        ]:
            with self.subTest(action=action.__name__):
                await self.violation(action, code)

    async def test_A_token_and_padding_are_tolerated(self):
        async with self.session() as s:
            token = w.vi(3) + w.vi(0) + b"opaque"
            sub = await s.sub.subscribe(NS, b"ai/events", [(w.P_TOKEN, token)])  # A15 USE_VALUE
            padding = s.sub._quic.get_next_available_stream_id(is_unidirectional=True)
            s.sub.send_raw(padding, w.vi(w.PADDING_STREAM) + bytes(64), end=True)
            s.pub.publish(EVENTS, 1, b"x")
            await self.settle(s, lambda: len(sub.objects) == 1)
            self.assertIsNone(s.pub.closed_code)

    async def test_C_new_group_request(self):
        async with self.session() as s:
            sub = await s.sub.subscribe(STATUS_NS, b"health", [(w.P_NEW_GROUP, 0)])  # C13
            self.assertEqual(sub.ok["properties"][w.X_DYNAMIC_GROUPS], 1)
            await s.pub.until(lambda: s.pub.seen("new_group"))
            s.pub.publish(HEALTH, 5, b"h")
            await s.sub.update(sub, [(w.P_NEW_GROUP, 5)])
            self.assertEqual(len(s.pub.seen("new_group")), 1)  # not larger than the current Group
            await s.sub.update(sub, [(w.P_NEW_GROUP, 6)])
            self.assertEqual(len(s.pub.seen("new_group")), 2)
            await s.sub.subscribe(NS, b"ai/events", [(w.P_NEW_GROUP, 0)])
            self.assertEqual(len(s.pub.seen("new_group")), 2)  # the track has no dynamic groups


    async def test_X1_limits_are_known_before_asking(self):
        """Extension X1: a constrained publisher states its limits in SETUP."""
        limits = dict(message=300, subscriptions=2, fetches=1)
        async with self.session(publisher=limits) as s:
            self.assertEqual(s.sub.peer_limits[w.OPT_MAX_CONTROL_MESSAGE], 300)
            self.assertEqual(s.sub.peer_limits[w.OPT_MAX_SUBSCRIPTIONS], 2)
            self.assertEqual(s.sub.peer_limits[w.OPT_MAX_FETCHES], 1)
            await s.sub.subscribe(NS, b"ai/events")
            await s.sub.subscribe(NS, b"preview/jpeg")
            with self.assertRaises(LimitReached):
                await s.sub.subscribe(NS, b"evidence/jpeg")
            with self.assertRaises(LimitReached):
                await s.sub.track_status(NS, b"x" * 400)
            s.pub.fetch_delay = 0.1
            for group in range(4):
                s.pub.publish(EVIDENCE, group, b"e%d" % group)
            fetched = await asyncio.gather(
                *(s.sub.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(g, 0, 0))]) for g in range(4))
            )
            self.assertEqual([f.objects[0].payload for f in fetched], [b"e0", b"e1", b"e2", b"e3"])
            self.assertEqual(s.sub.local_refusals, 2)
            self.assertEqual(s.sub.seen("request_error"), [])  # nothing was refused on the wire
            self.assertIsNone(s.pub.closed_code)

    async def test_X1_without_the_extension(self):
        """The same load against draft-21 alone: the limits show up as errors and a closed session."""
        async with self.session(extensions=False) as s:
            self.assertEqual(s.sub.peer_limits, {})
            s.pub.fetch_delay = 0.1
            for group in range(6):
                s.pub.publish(EVIDENCE, group, b"e%d" % group)
            results = await asyncio.gather(
                *(s.sub.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(g, 0, 0))]) for g in range(6)),
                return_exceptions=True,
            )
            refused = [r for r in results if isinstance(r, RequestFailed)]
            self.assertTrue(refused)
            self.assertTrue(all(r.code == 0x9 for r in refused))  # EXCESSIVE_LOAD after a round trip
            self.assertEqual(len(s.sub.seen("request_error")), len(refused))
            # A control message above the internal buffer of the device ends the whole session.
            s.sub.send_raw(s.sub.new_request_stream(), w.track_status(101, NS, b"x" * 1500))
            await self.settle(s, lambda: s.sub.terminated is not None)
            self.assertEqual(s.sub.terminated["code"], 0x1)

    async def test_X1_publisher_honours_subscriber_limits(self):
        async with self.session(own_limits={w.OPT_MAX_SUBSCRIPTIONS: 1}) as s:
            self.assertEqual(s.pub.publish_track(EVIDENCE), 0)
            self.assertEqual(s.pub.publish_track(PREVIEW), -1)  # refused before anything is sent
            await self.settle(s, lambda: len(s.sub.seen("publish")) == 1)
            await asyncio.sleep(0.1)
            self.assertEqual(len(s.sub.seen("publish")), 1)

    async def test_X2_source_delivery_timeout(self):
        """Extension X2: the deadline counts from sensing, not from the hand-over to MOQT."""
        tracks = [dict(t) for t in __import__("host_core").DEFAULT_TRACKS]
        tracks[EVIDENCE] = dict(tracks[EVIDENCE], object_timeout_ms=2000, source_timeout_ms=5000)
        async with self.session(publisher=dict(source_timeout=True), tracks=tracks) as s:
            self.assertEqual(s.sub.peer_limits[w.OPT_SOURCE_TIMEOUT], 1)
            by_object = await s.sub.subscribe(NS, b"evidence/jpeg", [(w.P_FORWARD, 1)])
            self.assertEqual(by_object.ok["properties"][w.X_SOURCE_TIMEOUT], 5000)
            # Inference and conversion took 3 s before the image reached MOQT.
            d = s.pub.publish(EVIDENCE, 1, b"img", source_age_ms=3000, props=w.kvp([(0x78, 1)]))
            self.assertEqual((d[0]["object_timeout_ms"], d[0]["source_timeout_ms"]), (2000, 5000))
            await self.settle(s, lambda: len(by_object.objects) == 1)
            self.assertEqual(by_object.objects[0].properties, {0x78: 1, w.X_SOURCE_AGE: 3000})
            s.sub.cancel(by_object)
            await s.pub.until(lambda: s.pub.active(EVIDENCE) == 0)
            strict = await s.sub.subscribe(NS, b"evidence/jpeg", [(w.P_SOURCE_TIMEOUT, 2000)])
            # OBJECT_DELIVERY_TIMEOUT alone would send this object: its 2000 ms start at the offer.
            self.assertEqual(s.pub.publish(EVIDENCE, 2, b"old", source_age_ms=3000), [])
            self.assertEqual(s.pub.source_expired(), 1)
            d = s.pub.publish(EVIDENCE, 3, b"fresh", source_age_ms=500)
            self.assertEqual(d[0]["source_timeout_ms"], 2000)
            # 500 ms old and 1 s in the queue: 1500 ms, still inside 2000 ms.
            s.pub.hold[EVIDENCE] = 1.0
            s.pub.publish(EVIDENCE, 4, b"waits", source_age_ms=500)
            # 1200 ms old and 1 s in the queue: 2200 ms, reset with DELIVERY_TIMEOUT.
            s.pub.publish(EVIDENCE, 5, b"late", source_age_ms=1200)
            await self.settle(s, lambda: len(strict.objects) == 2 and len(strict.resets) == 1)
            self.assertEqual([o.group for o in strict.objects], [3, 4])
            self.assertEqual(strict.resets[0], dict(group=5, code=0x2))
            self.assertEqual(strict.objects[0].properties[w.X_SOURCE_AGE], 500)
            # The retained object reports its age when it is fetched.
            await asyncio.sleep(0.3)
            s.pub.tick()
            f = await s.sub.fetch(NS, b"evidence/jpeg", [(w.P_LOCATION, w.location_filter(3, 0, 0))])
            self.assertGreaterEqual(f.objects[0].properties[w.X_SOURCE_AGE], 800)
            self.assertFalse(s.sub.errors)

    async def test_X2_not_negotiated(self):
        async with self.session() as s:  # the publisher does not state support
            self.assertNotIn(w.OPT_SOURCE_TIMEOUT, s.sub.peer_limits)
            with self.assertRaises(LimitReached):
                await s.sub.subscribe(NS, b"evidence/jpeg", [(w.P_SOURCE_TIMEOUT, 2000)])
            sub = await s.sub.subscribe(NS, b"evidence/jpeg")
            self.assertEqual(len(s.pub.publish(EVIDENCE, 1, b"x", source_age_ms=99999)), 1)
            await self.settle(s, lambda: len(sub.objects) == 1)
            self.assertNotIn(w.X_SOURCE_AGE, sub.objects[0].properties)
            self.assertNotIn(w.X_SOURCE_TIMEOUT, sub.ok["properties"])
            # Sent anyway, the parameter is unknown to the publisher and ends the session.
            s.sub.send_raw(s.sub.new_request_stream(), w.subscribe(101, NS, b"ai/events", [(w.P_SOURCE_TIMEOUT, 1)]))
            await self.settle(s, lambda: s.sub.terminated is not None)
            self.assertEqual(s.sub.terminated["code"], 0x3)

    async def test_C_expires_and_state_notify(self):
        async with self.session() as s:
            s.pub.set_lifetime(300)
            sub = await s.sub.subscribe(NS, b"ai/events")
            self.assertEqual(sub.ok["params"][w.P_EXPIRES], 300)  # C14
            s.pub.publish(EVENTS, 3, b"r", object_id=1, subgroup=1)
            self.assertEqual(s.pub.state_notify(0), 0)  # C10
            await self.settle(s, lambda: getattr(sub, "notifications", []))
            self.assertEqual(sub.notifications[0][w.P_LARGEST], (3, 1))
            self.assertEqual(sub.notifications[0][w.P_FORWARD], 1)
            await asyncio.sleep(0.2)
            ok = await s.sub.update(sub, [])  # an update extends the subscription
            self.assertEqual(ok["params"][w.P_EXPIRES], 300)
            await asyncio.sleep(0.2)
            s.pub.tick()
            self.assertEqual(s.pub.active(EVENTS), 1)
            await asyncio.sleep(0.2)
            s.pub.tick()
            await asyncio.wait_for(sub.finished.wait(), 5)
            self.assertEqual(sub.done["status"], 0x6)  # EXPIRED

    async def test_B_mandatory_track_property(self):
        s = Subscriber.__new__(Subscriber)
        self.assertTrue(s.unsupported({0x4000: 1}))  # B13
        self.assertFalse(s.unsupported({w.X_PRIORITY: 64, 0x7A: 1, 0x8000: 1}))

    async def test_Z_experiment_receiver_fetches_evidence(self):
        """receiver_draft21.py against the firmware core: evidence on demand by Group ID."""
        import json

        from receiver_draft21 import EVIDENCE_REQUIRED, ExperimentReceiver
        from PIL import Image
        import io

        def jpeg(shade):
            b = io.BytesIO()
            Image.new("RGB", (32, 24), (shade, shade, shade)).save(b, "JPEG")
            return b.getvalue()

        out = Path(self.directory.name) / "experiment"
        out.mkdir()
        port = free_port()
        from aioquic.asyncio import connect, serve
        from aioquic.quic.configuration import QuicConfiguration

        config = QuicConfiguration(
            is_client=False, alpn_protocols=[ALPN], max_datagram_frame_size=1200, max_data=2**22, max_stream_data=2**20
        )
        config.load_cert_chain(self.cert, self.key)
        receivers = []

        def make(*a, **kw):
            r = ExperimentReceiver(
                *a,
                out=out,
                authority="localhost:14568",
                evidence="fetch",
                image_track=b"evidence/jpeg",
                status=True,
                fetch_retry_s=0.05,
                fetch_window_s=2.0,
                **kw,
            )
            receivers.append(r)
            return r

        server = await serve("127.0.0.1", port, configuration=config, create_protocol=make)
        client = QuicConfiguration(is_client=True, alpn_protocols=[ALPN], max_datagram_frame_size=1200, server_name="localhost")
        client.load_verify_locations(str(self.cert))
        try:
            tracks = [dict(t) for t in __import__("host_core").DEFAULT_TRACKS]
            tracks[3] = dict(ns=(b"aiot", b"esp32"), name=b"status", priority=224, dynamic_groups=True)
            async with connect(
                "127.0.0.1", port, configuration=client, create_protocol=lambda *a, **kw: CorePublisher(*a, tracks=tracks, **kw)
            ) as pub:
                await pub.until(lambda: len(pub.seen("subscribed")) == 2)
                self.assertEqual(pub.active(EVIDENCE), 0)  # nobody subscribes to evidence
                images = {}
                for group in range(1, 7):
                    detections = 1 if group in (2, 5) else 0
                    result = json.dumps(dict(schema=1, seq=group, backend=2, detections=detections)).encode()
                    props = w.kvp([(0x78, 1000 * group), (0x7A, detections), (EVIDENCE_REQUIRED, detections)])
                    # The result is sent first; its evidence is retained a moment later.
                    pub.publish(EVENTS, group, result, props=props, priority=32 if detections else None)
                    if detections:
                        await asyncio.sleep(0.12)
                        images[group] = jpeg(40 * group)
                        self.assertEqual(pub.publish(EVIDENCE, group, images[group], props=w.kvp([(0x78, 1000 * group)])), [])
                    pub.publish(3, group, json.dumps(dict(heap=group)).encode(), datagram=True, retain=False)
                r = receivers[0]
                await r.until(lambda: len(r.seen("fetch_object")) == 2, 5)
                pub.end_track(EVENTS)
                pub.end_track(3)
                await r.until(lambda: r.all_finished, 5)
        finally:
            server.close()
        events = [json.loads(x) for x in (out / "events.jsonl").read_text().splitlines()]
        fetched = {e["group"]: e for e in events if e["event"] == "fetch_object"}
        self.assertEqual(sorted(fetched), [2, 5])
        for group, e in fetched.items():
            self.assertEqual((out / f"evidence-{group:06d}.jpg").read_bytes(), images[group])
            self.assertEqual((e["width"], e["height"], e["alias"]), (32, 24, 3))
            self.assertGreaterEqual(e["attempts"], 2)  # the first request preceded the evidence
        results = [e for e in events if e["event"] == "object" and e["alias"] == 2]
        self.assertEqual([e["group"] for e in results], list(range(1, 7)))
        self.assertEqual([e["priority"] for e in results], [None, 32, None, None, 32, None])
        self.assertEqual(len([e for e in events if e["event"] == "object" and e["alias"] == 4]), 6)
        self.assertFalse([e for e in events if e["event"] in ("error", "fetch_failed")])
        self.assertTrue([e for e in events if e["event"] == "finished"])

    async def experiment(self, name, scenario, **receiver_options):
        """Runs `scenario(pub, receiver)` against receiver_draft21.ExperimentReceiver."""
        import json

        from aioquic.asyncio import connect, serve
        from aioquic.quic.configuration import QuicConfiguration
        from receiver_draft21 import ExperimentReceiver

        out = Path(self.directory.name) / name
        out.mkdir()
        port = free_port()
        config = QuicConfiguration(
            is_client=False, alpn_protocols=[ALPN], max_datagram_frame_size=1200, max_data=2**22, max_stream_data=2**20
        )
        config.load_cert_chain(self.cert, self.key)
        receivers = []
        options = dict(
            evidence="none", image_track=b"evidence/jpeg", status=False, fetch_retry_s=0.05, fetch_window_s=2.0,
            recover_grace_s=0.1,
        )
        options.update(receiver_options)

        def make(*a, **kw):
            r = ExperimentReceiver(*a, out=out, authority="localhost:14568", **options, **kw)
            receivers.append(r)
            return r

        server = await serve("127.0.0.1", port, configuration=config, create_protocol=make)
        client = QuicConfiguration(is_client=True, alpn_protocols=[ALPN], max_datagram_frame_size=1200, server_name="localhost")
        client.load_verify_locations(str(self.cert))
        try:
            async with connect("127.0.0.1", port, configuration=client, create_protocol=CorePublisher) as pub:
                await pub.until(lambda: pub.seen("subscribed"))
                await scenario(pub, receivers[0])
        finally:
            server.close()
        return [json.loads(x) for x in (out / "events.jsonl").read_text().splitlines()]

    @staticmethod
    def result(pub, group, new, total, **kw):
        import json

        payload = json.dumps(dict(schema=1, seq=group, backend=2, passages_new=new, passages_total=total)).encode()
        props = w.kvp([(0x78, group * 100), (0x7A, new), (0x3800, total), (0x3802, new)])
        return pub.publish(EVENTS, group, payload, props=props, announce_gap=True, **kw)

    async def test_M_results_that_did_not_arrive_are_fetched(self):
        """Mission: no passage event is lost. Group IDs are source sequence numbers."""

        async def scenario(pub, r):
            self.result(pub, 3, 1, 1)
            self.result(pub, 29, 2, 3, deliver=False)  # the stream slot was busy: retained only
            self.result(pub, 55, 0, 3, deliver=False)
            self.result(pub, 81, 1, 4)  # reveals that 55 exists
            await r.until(lambda: len(r.gaps.received) == 4, 5)
            self.assertEqual(r.gaps.frontier, 81)
            self.result(pub, 107, 1, 5)
            self.result(pub, 133, 2, 7, deliver=False)  # nothing after it reveals it
            await r.until(lambda: 107 in r.gaps.received, 5)
            pub.end_track(EVENTS)
            await r.until(lambda: r.all_finished, 5)

        events = await self.experiment("recover", scenario)
        results = {e["group"]: e for e in events if e["event"] == "object" and e["alias"] == 2}
        self.assertEqual(sorted(results), [3, 29, 55, 81, 107, 133])
        self.assertEqual(sorted(g for g, e in results.items() if e["recovered"]), [29, 55, 133])
        self.assertEqual([e["missing"] for e in events if e["event"] == "gap_detected"], [55])
        finished = next(e for e in events if e["event"] == "finished")
        self.assertEqual((finished["results"], finished["recovered"], finished["lost"]), (6, 3, 0))
        self.assertEqual((finished["passage_events"], finished["passages_total"]), (7, 7))
        self.assertFalse([e for e in events if e["event"] == "error"])
        # PUBLISH_DONE counts the three streams that were opened.
        done = next(e for e in events if e["event"] == "publish_done")
        self.assertEqual(done["streams"], 3)

    async def test_M_without_recovery_the_events_are_missing(self):
        async def scenario(pub, r):
            self.result(pub, 3, 1, 1)
            self.result(pub, 29, 2, 3, deliver=False)
            self.result(pub, 81, 1, 4)
            await r.until(lambda: len(r.gaps.received) == 2, 5)
            pub.end_track(EVENTS)
            await r.until(lambda: r.all_finished, 5)

        events = await self.experiment("norecover", scenario, recover=False)
        finished = next(e for e in events if e["event"] == "finished")
        self.assertEqual((finished["results"], finished["recovered"]), (2, 0))
        # The cumulative counter still reports the total; the two events of Group 29 are unknown.
        self.assertEqual((finished["passage_events"], finished["passages_total"]), (2, 4))
        self.assertEqual([e["missing"] for e in events if e["event"] == "gap_detected"], [29])

    async def test_M_a_result_the_device_no_longer_holds_is_reported_lost(self):
        async def scenario(pub, r):
            self.result(pub, 3, 1, 1)
            self.result(pub, 29, 1, 2, deliver=False, retain=False)  # evicted before anyone asked
            self.result(pub, 81, 1, 3)
            await r.until(lambda: r.seen("lost"), 5)
            pub.end_track(EVENTS)
            await r.until(lambda: r.all_finished, 5)

        events = await self.experiment("lost", scenario)
        self.assertEqual([e["group"] for e in events if e["event"] == "lost"], [29])
        finished = next(e for e in events if e["event"] == "finished")
        self.assertEqual((finished["results"], finished["lost"]), (2, 1))
        self.assertEqual((finished["passage_events"], finished["passages_total"]), (2, 3))

    async def test_M_gap_tracker(self):
        from receiver_draft21 import GapTracker

        t = GapTracker()
        for group, gap in [(3, 3), (29, 25)]:
            t.remember(group, gap)
            self.assertIsNone(t.add(group, gap, 0.0))
        self.assertEqual(t.frontier, 29)
        t.remember(107, 25)
        self.assertEqual(t.add(107, 25, 1.0), 81)  # 81 precedes 107 and did not arrive
        self.assertEqual(t.frontier, 29)
        self.assertEqual(t.due(1.2, 0.5), [])  # still inside the reordering grace period
        self.assertEqual(t.due(1.6, 0.5), [81])
        t.remember(81, 25)
        self.assertEqual(t.add(81, 25, 1.7), 55)
        t.remember(55, 25)
        self.assertIsNone(t.add(55, 25, 1.8))  # its predecessor 29 is known
        self.assertEqual((t.frontier, t.expected), (107, {}))
        # An object without the property reveals nothing.
        self.assertIsNone(t.add(200, None, 2.0))
        self.assertEqual(t.frontier, 107)

    async def test_Z_legacy_receiver_accepts_new_publisher(self):
        """receiver.py --dual, used by the archived experiment harness."""
        import json

        from receiver import DualReceiver
        from PIL import Image
        import io
        from aioquic.asyncio import connect, serve
        from aioquic.quic.configuration import QuicConfiguration

        b = io.BytesIO()
        Image.new("RGB", (16, 16), (9, 9, 9)).save(b, "JPEG")
        out = Path(self.directory.name) / "legacy"
        out.mkdir()
        port = free_port()
        config = QuicConfiguration(is_client=False, alpn_protocols=[ALPN], max_datagram_frame_size=1200)
        config.load_cert_chain(self.cert, self.key)
        server = await serve(
            "127.0.0.1",
            port,
            configuration=config,
            create_protocol=lambda *a, **kw: DualReceiver(*a, out=out, authority="localhost:14568", **kw),
        )
        client = QuicConfiguration(is_client=True, alpn_protocols=[ALPN], max_datagram_frame_size=1200, server_name="localhost")
        client.load_verify_locations(str(self.cert))
        try:
            async with connect("127.0.0.1", port, configuration=client, create_protocol=CorePublisher) as pub:
                await pub.until(lambda: len(pub.seen("subscribed")) == 2)
                for group in range(3):
                    wire = (b"\xd1\x02\x02" + bytes(17)).hex()
                    result = json.dumps(dict(schema=1, seq=group, backend=2, result_hex=wire)).encode()
                    pub.publish(EVENTS, group, result, priority=32 if group == 1 else None)
                    pub.publish(PREVIEW, group, b.getvalue(), props=w.kvp([(0x78, group)]))
                pub.end_track(PREVIEW)
                pub.end_track(EVENTS)
                await pub.until(lambda: len(pub.seen("request_closed")) == 2)
        finally:
            server.close()
        events = [json.loads(x) for x in (out / "events.jsonl").read_text().splitlines()]
        self.assertFalse([e for e in events if e["event"] == "error"])
        self.assertEqual(len([e for e in events if e["event"] == "object" and e.get("alias") == 1]), 3)
        self.assertEqual(len([e for e in events if e["event"] == "object" and e.get("alias") == 2]), 3)
        self.assertTrue([e for e in events if e["event"] == "finished"])


if __name__ == "__main__":
    unittest.main()
