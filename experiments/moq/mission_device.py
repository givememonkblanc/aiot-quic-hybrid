"""Host model of the device for the mission comparison.

The protocol logic is the firmware's C core. What is modelled here is the part of the
firmware that lives in moqt_ngtcp2.inc and app_moq_concurrent.inc: the result queue, one
stream slot for results and one for images, the order in which slots are served, the
bound on unacknowledged image bytes, deadlines and reconnection.

This is a model. It shows how the alternatives behave under the same load and link; it
does not measure the ESP32."""

import asyncio
import io
import json
import random
import time

from aioquic.asyncio import connect
from aioquic.quic.configuration import QuicConfiguration
from PIL import Image

import moqt_wire as w
from host_core import CorePublisher

EVENTS, EVIDENCE = 1, 2
ALPN = "moqt-21"
POLICIES = {
    "A": dict(name="draft default", results_first=False, deadline_results=False, deadline_images=False,
              retain=False, recover=False),
    "B": dict(name="delivery timeouts", results_first=True, deadline_results=True, deadline_images=True,
              retain=False, recover=False),
    "C": dict(name="results first", results_first=True, deadline_results=False, deadline_images=False,
              retain=False, recover=False),
    "D": dict(name="mission profile", results_first=True, deadline_results=False, deadline_images=True,
              retain=True, recover=True),
}


def jpeg(size, seed=7):
    """A valid JPEG of roughly `size` bytes."""
    rng = random.Random(seed)
    side = 32
    while True:
        im = Image.frombytes("RGB", (side, side), bytes(rng.getrandbits(8) for _ in range(side * side * 3)))
        b = io.BytesIO()
        im.save(b, "JPEG", quality=70)
        if b.tell() >= size or side > 400:
            return b.getvalue()
        side += 8


class Slot:
    def __init__(self, stream, data, item, admitted):
        self.stream, self.data, self.item, self.admitted = stream, data, item, admitted
        self.written = 0


class Device:
    def __init__(self, policy, port, cert, *, inference_s=0.4, produce_s=14.0, drain_s=8.0, deadline_s=3.0,
                 image_bytes=12000, queue_results=32, unacked_image=32 * 1024, slice_bytes=1200, idle_timeout=3.0,
                 seed=1, host="127.0.0.1", retry_s=0.3, attempt_s=None, events=(0, 0, 1, 1, 2)):
        self.p = POLICIES[policy]
        self.policy = policy
        self.port, self.cert, self.host, self.retry_s, self.attempt_s = port, cert, host, retry_s, attempt_s
        self.events = list(events)
        self.inference_s, self.produce_s, self.drain_s = inference_s, produce_s, drain_s
        self.deadline_s, self.queue_results = deadline_s, queue_results
        self.unacked_image, self.slice_bytes, self.idle_timeout = unacked_image, slice_bytes, idle_timeout
        self.image = jpeg(image_bytes)
        self.rng = random.Random(seed)
        self.state = {}
        self.ledger = []
        self.results = []  # waiting results, oldest first
        self.pending_image = None  # only the latest image waits, as in the firmware
        self.slots = {"result": None, "image": None}
        self.offline, self.retain_results, self.backlog = [], 32, False
        self.stats = dict(sessions=0, results_expired=0, results_overflow=0, results_retained_only=0,
                          results_retained_offline=0, results_retained_backlog=0,
                          results_requeued=0, images_offered=0, images_replaced=0, images_expired=0,
                          images_reset=0, results_reset=0, image_bytes_written=0, result_bytes_written=0)
        self.total = 0
        self.sequence = 0

    # -- workload ------------------------------------------------------------
    def produce(self, now):
        self.sequence += 26  # source sequence of the inferred frame; not contiguous
        new = self.rng.choice(self.events)
        self.total += new
        item = dict(group=self.sequence, capture_t=now, passages_new=new, passages_total=self.total,
                    deadline=now + self.deadline_s)
        self.ledger.append(dict(item))
        if len(self.results) < self.queue_results:
            self.results.append(item)
        elif self.p["retain"] and len(self.offline) < self.retain_results:
            # As in the firmware: without a session the oldest waiting result moves to the
            # retained set, and the next session serves it through FETCH.
            self.offline.append(self.results.pop(0))
            self.results.append(item)
            self.stats["results_retained_offline"] += 1
        else:
            self.stats["results_overflow"] += 1
        if new:
            self.stats["images_offered"] += 1
            if self.pending_image is not None:
                self.stats["images_replaced"] += 1
            self.pending_image = item

    async def producer(self, start):
        k = 0
        while True:
            due = start + k * self.inference_s
            if due - start >= self.produce_s:
                return
            await asyncio.sleep(max(0.0, due - time.monotonic()))
            self.produce(time.monotonic())
            k += 1

    # -- one session -----------------------------------------------------------
    def result_payload(self, item):
        return json.dumps(dict(schema=1, seq=item["group"], backend=2, passages_new=item["passages_new"],
                               passages_total=item["passages_total"])).encode()

    def result_props(self, item):
        return w.kvp([(0x78, int(item["capture_t"] * 1000) % 2**31), (0x7A, item["passages_new"]),
                      (0x7C, 1 if item["passages_new"] else 0), (0x3800, item["passages_total"]),
                      (0x3802, item["passages_new"])])

    def open_slot(self, pub, kind, head, payload, item, now):
        sid = pub._quic.get_next_available_stream_id(is_unidirectional=True)
        slot = self.slots[kind] = Slot(sid, head + payload, item, now)
        # The stream header goes out at once; it also reserves the stream ID.
        pub._quic.send_stream_data(sid, head)
        slot.written = len(head)

    def admit(self, pub, now):
        p = self.p
        while self.offline:
            item = self.offline.pop(0)
            pub.publish(EVENTS, item["group"], self.result_payload(item), props=self.result_props(item),
                        announce_gap=True, retain=True, deliver=False, send=False)
        if p["retain"] and self.backlog:
            # After a reconnection the newest result goes first, because it carries the count.
            # The results that waited are retained, and the receiver fetches them in one stream.
            self.backlog = False
            while len(self.results) > 1 and self.slots["result"] is None:
                item = self.results.pop(0)
                pub.publish(EVENTS, item["group"], self.result_payload(item), props=self.result_props(item),
                            announce_gap=True, retain=True, deliver=False, send=False)
                self.stats["results_retained_backlog"] += 1
        while self.results:
            item = self.results[0]
            if p["deadline_results"] and now >= item["deadline"]:
                self.results.pop(0)
                self.stats["results_expired"] += 1
                continue
            busy = self.slots["result"] is not None
            # As in the firmware: a result waits for the stream slot. With retention it moves
            # to the FETCH cache only when the queue is three quarters full.
            crowded = p["retain"] and len(self.results) * 4 >= self.queue_results * 3
            if busy and not crowded:
                break
            d = pub.publish(EVENTS, item["group"], self.result_payload(item), props=self.result_props(item),
                            announce_gap=p["retain"], retain=p["retain"], deliver=not busy, send=False)
            self.results.pop(0)
            if busy:
                self.stats["results_retained_only"] += 1
                continue
            if d:
                pub.lib.moqt_core_stream_opened(pub.core, d[0]["sub"])
                self.open_slot(pub, "result", d[0]["head"], self.result_payload(item), item, now)
            break
        item = self.pending_image
        if item is not None and self.slots["image"] is None:
            self.pending_image = None
            if (p["deadline_images"]) and now >= item["deadline"]:
                self.stats["images_expired"] += 1
                return
            d = pub.publish(EVIDENCE, item["group"], self.image, props=w.kvp([(0x78, 1)]), retain=False, send=False)
            if d:
                pub.lib.moqt_core_stream_opened(pub.core, d[0]["sub"])
                self.open_slot(pub, "image", d[0]["head"], self.image, item, now)

    def sender(self, pub, slot):
        stream = pub._quic._streams.get(slot.stream)
        return stream.sender if stream is not None else None

    def serve(self, pub, now):
        p = self.p
        for kind, slot in list(self.slots.items()):
            if slot is None:
                continue
            sender = self.sender(pub, slot)
            if slot.written == len(slot.data) and sender is not None and sender.is_finished:
                self.slots[kind] = None
                continue
            expires = p["deadline_results"] if kind == "result" else p["deadline_images"]
            if expires and now >= slot.item["deadline"]:
                if slot.written:
                    pub._quic.reset_stream(slot.stream, 0x2)
                else:
                    pub._quic.send_stream_data(slot.stream, slot.data[:3])
                    pub._quic.reset_stream(slot.stream, 0x2)
                self.stats["results_reset" if kind == "result" else "images_reset"] += 1
                self.slots[kind] = None
        ready = []
        for kind, slot in self.slots.items():
            if slot is None or slot.written == len(slot.data):
                continue
            sender = self.sender(pub, slot)
            acked = sender._buffer_start if sender is not None else 0
            if kind == "image" and slot.written - acked >= self.unacked_image:
                continue
            ready.append((kind, slot))
        if not ready:
            return
        if p["results_first"]:
            ready.sort(key=lambda x: (x[0] != "result", x[1].admitted))
        else:
            ready.sort(key=lambda x: x[1].admitted)
        kind, slot = ready[0]
        chunk = slot.data[slot.written : slot.written + self.slice_bytes]
        slot.written += len(chunk)
        pub._quic.send_stream_data(slot.stream, chunk, end_stream=slot.written == len(slot.data))
        self.stats["result_bytes_written" if kind == "result" else "image_bytes_written"] += len(chunk)

    def connection_lost(self):
        """Objects that were in a stream are not known to have arrived. A result goes back to
        the head of the queue; with retention the subscriber fetches it instead."""
        slot = self.slots["result"]
        if slot is not None and not self.p["retain"]:
            self.results.insert(0, slot.item)
            self.stats["results_requeued"] += 1
        self.slots = {"result": None, "image": None}
        self.backlog = True

    async def session(self, stop_at, tracks):
        config = QuicConfiguration(is_client=True, alpn_protocols=[ALPN], max_datagram_frame_size=1200,
                                   server_name="localhost", idle_timeout=self.idle_timeout)
        config.load_verify_locations(str(self.cert))
        try:
            async with connect(self.host, self.port, configuration=config, wait_connected=self.attempt_s is None,
                               create_protocol=lambda *a, **kw: CorePublisher(*a, state=self.state, tracks=tracks, **kw)
                               ) as pub:
                if self.attempt_s is not None:
                    # A handshake that meets an outage backs off; the attempt is bounded.
                    pub.transmit()
                    await asyncio.wait_for(pub.wait_connected(), self.attempt_s)
                self.stats["sessions"] += 1
                await pub.until(lambda: pub.seen("subscribed") or pub.peer_closed is not None, 4)
                try:
                    # The receiver subscribes to both tracks; a track that ends in between
                    # would refuse the second subscription.
                    await pub.until(lambda: len(pub.seen("subscribed")) >= 2 or pub.peer_closed is not None, 1)
                except (TimeoutError, asyncio.TimeoutError):
                    pass
                while pub.peer_closed is None and pub.closed_code is None:
                    now = time.monotonic()
                    if now >= stop_at and not self.results and not any(self.slots.values()):
                        break
                    if now >= stop_at + self.drain_s:
                        break
                    self.admit(pub, now)
                    self.serve(pub, now)
                    pub.transmit()
                    await asyncio.sleep(0.005)
                if pub.peer_closed is None and pub.closed_code is None:
                    pub.end_track(EVENTS)
                    pub.end_track(EVIDENCE)
                    try:
                        # The receiver closes a subscription after it fetched what it missed;
                        # request streams of FETCH close earlier and do not count.
                        subscriptions = {e["stream"] for e in pub.seen("subscribed")}
                        await pub.until(lambda: subscriptions <= {e["stream"] for e in pub.seen("request_closed")}
                                        or pub.peer_closed is not None, 8)
                    except TimeoutError:
                        pass
                    return True
        except (ConnectionError, TimeoutError, asyncio.TimeoutError, OSError):
            pass
        self.connection_lost()
        return False

    async def run(self, tracks=None, start=None):
        start = time.monotonic() if start is None else start
        await asyncio.sleep(max(0.0, start - time.monotonic()))
        producer = asyncio.ensure_future(self.producer(start))
        stop_at = start + self.produce_s
        hard_stop = stop_at + self.drain_s
        done = False
        while not done and time.monotonic() < hard_stop:
            done = await self.session(stop_at, tracks)
            if not done:
                await asyncio.sleep(self.retry_s)
        producer.cancel()
        self.stats["results_unsent"] = len(self.results)
        return start
