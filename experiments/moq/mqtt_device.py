#!/usr/bin/env python3
"""The counting workload of mission_device.py over MQTT, as a baseline outside MOQT.

The device publishes results with QoS 1 (or QoS 0) in a persistent session and images with
QoS 0 on a second topic of the same connection. The application keeps the same result queue
as the MOQT model, hands one result at a time to the client, sends results before images,
lets images expire, and starts a new connection attempt at the same interval. What differs
from MOQT is the transport: one TCP byte stream without per-object streams or priorities."""

import json
import random
import threading
import time

import paho.mqtt.client as mqtt
from paho.mqtt.properties import Properties
from paho.mqtt.packettypes import PacketTypes

from mission_device import jpeg

RESULTS, IMAGES = "aiot/esp32/ai/events", "aiot/esp32/evidence/jpeg"


class MqttDevice:
    def __init__(self, host, port, *, qos=1, inference_s=0.4, produce_s=14.0, drain_s=8.0, deadline_s=3.0,
                 image_bytes=12000, queue_results=32, keepalive_s=3, retry_s=1.0, connect_timeout_s=1.0, seed=1, events=(0, 0, 1, 1, 2)):
        self.host, self.port, self.qos = host, port, qos
        self.inference_s, self.produce_s, self.drain_s = inference_s, produce_s, drain_s
        self.deadline_s, self.queue_results = deadline_s, queue_results
        self.keepalive_s, self.retry_s, self.connect_timeout_s = keepalive_s, retry_s, connect_timeout_s
        self.image = jpeg(image_bytes)
        self.rng = random.Random(seed)
        self.events = list(events)
        self.lock = threading.Lock()
        self.ledger, self.results = [], []
        self.pending_image = None
        self.inflight = None  # (mid, item) of the result handed to the client
        self.image_mid = None
        self.connected = False
        self.total = self.sequence = 0
        self.stats = dict(sessions=0, results_expired=0, results_overflow=0, results_requeued=0, images_offered=0,
                          images_replaced=0, images_expired=0, images_sent=0, results_sent=0, connect_failures=0)
        c = self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="aiot-esp32", protocol=mqtt.MQTTv5)
        c.on_connect, c.on_disconnect, c.on_publish = self.on_connect, self.on_disconnect, self.on_publish
        c.on_connect_fail = self.on_connect_fail
        c.max_inflight_messages_set(2)
        c.connect_timeout = connect_timeout_s

    # The workload is that of mission_device.Device.produce.
    def produce(self, now):
        self.sequence += 26
        new = self.rng.choice(self.events)
        self.total += new
        item = dict(group=self.sequence, capture_t=now, passages_new=new, passages_total=self.total,
                    deadline=now + self.deadline_s)
        with self.lock:
            self.ledger.append(dict(item))
            if len(self.results) < self.queue_results:
                self.results.append(item)
            else:
                self.stats["results_overflow"] += 1
            if new:
                self.stats["images_offered"] += 1
                if self.pending_image is not None:
                    self.stats["images_replaced"] += 1
                self.pending_image = item

    def on_connect_fail(self, client, userdata):
        self.stats["connect_failures"] += 1

    def on_connect(self, client, userdata, flags, reason, properties):
        if not reason.is_failure:
            self.connected = True
            self.stats["sessions"] += 1

    def on_disconnect(self, client, userdata, flags, reason, properties):
        self.connected = False

    def on_publish(self, client, userdata, mid, reason, properties):
        with self.lock:
            if self.inflight is not None and self.inflight[0] == mid:
                self.inflight = None
            if self.image_mid == mid:
                self.image_mid = None

    def payload(self, item):
        return json.dumps(dict(schema=1, seq=item["group"], passages_new=item["passages_new"],
                               passages_total=item["passages_total"])).encode()

    def admit(self, now):
        with self.lock:
            if self.inflight is None and self.results:
                item = self.results.pop(0)
                info = self.client.publish(RESULTS, self.payload(item), qos=self.qos)
                self.stats["results_sent"] += 1
                if self.qos:
                    self.inflight = (info.mid, item)
                return
            item = self.pending_image
            if item is not None and self.image_mid is None and self.inflight is None:
                self.pending_image = None
                if now >= item["deadline"]:
                    self.stats["images_expired"] += 1
                    return
                head = json.dumps(dict(seq=item["group"])).encode()
                info = self.client.publish(IMAGES, bytes([len(head)]) + head + self.image, qos=0)
                self.image_mid = info.mid
                self.stats["images_sent"] += 1

    def connect(self):
        props = Properties(PacketTypes.CONNECT)
        props.SessionExpiryInterval = 3600
        # The client library opens the connection again by itself, one attempt per second.
        self.client.reconnect_delay_set(min_delay=1, max_delay=1)
        self.client.connect_async(self.host, self.port, keepalive=self.keepalive_s, clean_start=False, properties=props)

    def producer(self, start):
        k = 0
        while k * self.inference_s < self.produce_s:
            wait = start + k * self.inference_s - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            self.produce(time.monotonic())
            k += 1
        self.produced = True

    def run(self, start=None):
        start = time.monotonic() if start is None else start
        time.sleep(max(0.0, start - time.monotonic()))
        stop_at = start + self.produce_s
        hard_stop = stop_at + self.drain_s
        self.produced = False
        threading.Thread(target=self.producer, args=(start,), daemon=True).start()
        self.connect()
        self.client.loop_start()
        while True:
            now = time.monotonic()
            if self.connected:
                self.admit(now)
            else:
                # An image that was handed over is gone. A QoS 1 result is kept by the client
                # and sent again in the resumed session.
                with self.lock:
                    self.image_mid = None
            with self.lock:
                idle = not self.results and self.inflight is None and self.image_mid is None
            if (self.produced and idle and self.connected) or now >= hard_stop:
                break
            time.sleep(0.005)
        with self.lock:
            self.stats["results_unsent"] = len(self.results) + (1 if self.inflight else 0)
        time.sleep(0.3)
        self.client.disconnect()
        self.client.loop_stop()
        return start
