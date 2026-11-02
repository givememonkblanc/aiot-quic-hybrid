#!/usr/bin/env python3
"""Subscriber of the MQTT baseline (baseline_receiver.py): receives the mission of the device
(moq_mqtt_shim.c) from an MQTT broker and writes events.jsonl in the format of
receiver_draft21.py, so that the relay analysis reads both.

The session is persistent (clean session off), so that the broker keeps the QoS 1 results while
the connection is lost; images are QoS 0. Like the MOQT subscriber, a connection is given up
after about 5 s without traffic (keepalive) and a new attempt follows after --reconnect seconds.
A payload is [alias u8][group u32][properties length u16][Object Properties][object].

  subscribed  when the device is told to start (the retained start message), once per track
  object      every result (alias 2) and image (alias 3), with its properties
  finished    on the done message of the device"""
import argparse
import hashlib
import json
import ssl
import struct
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from moqt_wire import properties

RESULTS, IMAGES = "aiot/esp32/ai/events", "aiot/esp32/evidence/jpeg"
DONE, START = "aiot/esp32/ctl/done", "aiot/host/start"
HELLO = "aiot/esp32/ctl/hello"
# the MQTT baseline of the helper (app_moq_helper.inc, AIOT_MQTT_HELPER): its results, the JSON of [HELPER_DET]
HELPER = "aiot/esp32/helper/events"
HELPER_ALIAS = 5


def result_fields(alias, body):
    """The camera's result as receiver_draft21.describe logs it (binary AIOT_MOQT_RESULT_BINARY or JSON),
    so that the analyses read the same fields over MQTT; {} for other aliases or a malformed body."""
    if alias != 2:
        return {}
    try:
        if body[:1] == b"\xd1" and len(body) >= 20 and len(body) == 20 + 6 * body[3]:
            dets = [dict(zip(("class", "confidence", "x", "y", "w", "h"), body[20 + 6 * i:26 + 6 * i]))
                    for i in range(body[3])]
            return dict(result=dict(schema=2, seq=int.from_bytes(body[12:16], "little"), backend=body[2],
                                    detections=body[3], boxes=dets))
        return dict(result=json.loads(body))
    except (ValueError, IndexError):
        return {}
TRACKS = {2: "ai/events", 3: "evidence/jpeg"}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--connect", required=True, help="HOST:PORT of the broker")
    p.add_argument("--ca", help="certificate of the broker; plain TCP without it")
    p.add_argument("--client-id", required=True)
    p.add_argument("--tracks", choices=["results", "all"], default="all")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=150)
    p.add_argument("--keepalive", type=int, default=5)
    p.add_argument("--reconnect", type=float, default=1.0)
    p.add_argument("--tap", action="store_true", help="also record the hello messages of the device (first hop)")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    log = (a.out / "events.jsonl").open("a")
    lock = threading.Lock()
    state = dict(started=False, connected=False, sessions=0, done=False)

    def write(**rec):
        with lock:
            log.write(json.dumps(dict(t=time.monotonic(), **rec)) + "\n")
            log.flush()

    host, port = a.connect.rsplit(":", 1)
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=a.client_id, clean_session=False,
                    protocol=mqtt.MQTTv311)
    if a.ca:
        c.tls_set(ca_certs=a.ca, cert_reqs=ssl.CERT_REQUIRED)
        c.tls_insecure_set(True)    # the certificate names localhost
    c.reconnect_delay_set(a.reconnect, a.reconnect)
    c.connect_timeout = a.keepalive

    def on_connect(client, userdata, flags, reason, props):
        if reason.is_failure:
            write(event="error", reason=f"connect {reason}")
            return
        state["connected"] = True
        state["sessions"] += 1
        write(event="setup", implementation="mosquitto", session_present=bool(flags.session_present))
        subs = [(START, 1), (DONE, 1), (RESULTS, 1), (HELPER, 1)] + ([(IMAGES, 0)] if a.tracks == "all" else [])
        if a.tap:
            subs.append((HELLO, 1))
        client.subscribe(subs)

    def on_disconnect(client, userdata, flags, reason, props):
        if state["connected"]:
            write(event="session_lost", reason=str(reason))
        state["connected"] = False

    def on_message(client, userdata, msg):
        if msg.topic == START:
            if not state["started"]:
                state["started"] = True
                for alias in (2, 3) if a.tracks == "all" else (2,):
                    write(event="subscribed", alias=alias, track=TRACKS[alias], request=2 * (alias - 2))
            return
        if msg.topic == HELLO:
            write(event="device_hello")
            return
        if msg.topic == DONE:
            if not state["done"]:
                state["done"] = True
                write(event="publish_done", track="all")
                write(event="finished")
            return
        if msg.topic == HELPER:
            # as the relay's helper/events Object: group = the camera frame, property 0x78 = capture_ms
            try:
                h = json.loads(msg.payload)
            except ValueError:
                write(event="error", reason="helper payload")
                return
            write(event="object", alias=HELPER_ALIAS, group=int(h.get("group", 0)), object=0, subgroup=0, priority=None,
                  datagram=False, properties={"120": int(h.get("capture_ms", 0))}, bytes=len(msg.payload),
                  sha256=hashlib.sha256(msg.payload).hexdigest(), qos=msg.qos, dup=bool(msg.dup), recovered=False, helper=h)
            return
        b = msg.payload
        if len(b) < 7:
            write(event="error", reason="short payload")
            return
        alias, group, n = struct.unpack(">BIH", b[:7])
        try:
            props = properties(b[7:7 + n])
        except Exception as failure:   # noqa: BLE001 - a malformed payload is recorded, not fatal
            write(event="error", reason=f"properties {failure!r}")
            props = {}
        body = b[7 + n:]
        write(event="object", alias=alias, group=group, object=0, subgroup=0, priority=None, datagram=False,
              properties={str(k): v for k, v in props.items() if isinstance(v, int)}, bytes=len(body),
              sha256=hashlib.sha256(body).hexdigest(), qos=msg.qos, dup=bool(msg.dup), recovered=False,
              **result_fields(alias, body))

    c.on_connect, c.on_disconnect, c.on_message = on_connect, on_disconnect, on_message
    write(event="handshake", broker=a.connect)
    c.connect_async(host, int(port), keepalive=a.keepalive)
    c.loop_start()
    end = time.monotonic() + a.seconds
    try:
        while time.monotonic() < end and not state["done"]:
            time.sleep(0.1)
        # stay connected for a moment, so that the acknowledgement of the done message goes out
        time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        c.disconnect()
        c.loop_stop()
        write(event="closed", sessions=state["sessions"])
        log.close()


if __name__ == "__main__":
    main()
