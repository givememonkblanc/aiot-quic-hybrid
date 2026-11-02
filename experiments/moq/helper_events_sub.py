#!/usr/bin/env python3
"""Subscriber of the helper results through the relay (relay/moq_relay --helper-events).

The helper ESP32 publishes one Object per inference on its track aiot/esp32 helper/events; the
relay subscribes to it on the helper's session and serves it as its own track helper/events.
This client connects to the relay's downstream port (QUIC client, ALPN moqt-21) with the
subscriber of subscriber.py and sends

    SUBSCRIBE  namespace ("aiot", "esp32"), track name b"helper/events",
               parameters [(SUBSCRIBER_PRIORITY 0x20, 0)]   (no filter: from the next Object)

Every Object: Group = the camera frame sequence, property 0x78 = capture time (ms, camera
clock), payload = the JSON report of the helper ({"group","capture_ms","recv_us","start_us",
"end_us","dets":[[x,y],..],"n","ran",...}). Each is written as one JSON line
{"t": host CLOCK_MONOTONIC s, "group", "properties", "report"} to stdout and to --out.

usage: helper_events_sub.py [--connect 127.0.0.1:14600] [--seconds 60] [--out FILE]"""
import argparse
import asyncio
import json
import ssl
import time

from aioquic.asyncio import connect
from aioquic.quic.configuration import QuicConfiguration

import moqt_wire as w
from subscriber import ALPN, Subscriber

NS = (b"aiot", b"esp32")
TRACK = b"helper/events"


class HelperEvents(Subscriber):
    sink = None

    def deliver(self, obj, sid):
        before = sum(len(s.objects) for subs in self.by_alias.values() for s in subs)
        super().deliver(obj, sid)
        after = sum(len(s.objects) for subs in self.by_alias.values() for s in subs)
        if after == before or obj.status != w.STATUS_NORMAL:
            return
        try:
            report = json.loads(obj.payload)
        except ValueError:
            report = None
        rec = dict(t=time.monotonic(), group=obj.group, properties={str(k): v for k, v in obj.properties.items()
                                                                    if isinstance(v, int)}, report=report)
        if self.sink:
            self.sink(rec)


async def main(a):
    host, port = a.connect.rsplit(":", 1)
    config = QuicConfiguration(is_client=True, alpn_protocols=[ALPN], max_datagram_frame_size=1200,
                               server_name=host, idle_timeout=20.0)
    config.verify_mode = ssl.CERT_NONE
    out = open(a.out, "a") if a.out else None

    def sink(rec):
        line = json.dumps(rec)
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()

    end = time.monotonic() + a.seconds
    while time.monotonic() < end:
        try:
            async with connect(host, int(port), configuration=config, create_protocol=HelperEvents) as sub:
                sub.sink = sink
                await sub.until(lambda: sub.setup_done.is_set(), 5)
                await sub.subscribe(NS, TRACK, [(w.P_PRIORITY, 0)])
                print(json.dumps(dict(t=time.monotonic(), event="subscribed", track=TRACK.decode())), flush=True)
                while time.monotonic() < end and sub.terminated is None:
                    await asyncio.sleep(0.1)
        except (ConnectionError, OSError, TimeoutError, asyncio.TimeoutError, Exception) as e:  # noqa: BLE001
            print(json.dumps(dict(t=time.monotonic(), event="retry", reason=repr(e))), flush=True)
            await asyncio.sleep(1)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--connect", default="127.0.0.1:14600")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--out")
    asyncio.run(main(ap.parse_args()))
