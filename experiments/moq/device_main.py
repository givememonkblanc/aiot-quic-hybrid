#!/usr/bin/env python3
"""Runs one model device in the current network namespace and writes its ledger.

usage: device_main.py --protocol moqt|mqtt --policy A|B|C|D|M1|M0 --host H --port P --out DIR [options]"""

import argparse
import asyncio
import json
from pathlib import Path

TRACKS = [
    dict(ns=(b"aiot", b"esp32"), name=b"preview/jpeg", priority=224, group_order=2),
    dict(ns=(b"aiot", b"esp32"), name=b"ai/events", priority=32),
    dict(ns=(b"aiot", b"esp32"), name=b"evidence/jpeg", priority=128),
    dict(ns=(b"aiot", b"esp32"), name=b"status", priority=240),
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--policy", required=True)
    p.add_argument("--host", required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--cert", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--produce", type=float, default=14.0)
    p.add_argument("--drain", type=float, default=8.0)
    p.add_argument("--inference", type=float, default=0.4)
    p.add_argument("--deadline", type=float, default=3.0)
    p.add_argument("--image-bytes", type=int, default=12000)
    p.add_argument("--queue-results", type=int, default=32)
    p.add_argument("--idle-timeout", type=float, default=3.0)
    p.add_argument("--retry", type=float, default=1.0)
    p.add_argument("--attempt", type=float, default=2.0, help="longest connection attempt, seconds")
    p.add_argument("--events", default="0,0,0,1", help="new passage events of a result, drawn uniformly")
    p.add_argument("--start-at", type=float, help="time.monotonic() at which the workload starts")
    p.add_argument("--unacked-image", type=int, default=32 * 1024, help="image bytes written and not acknowledged, at most")
    a = p.parse_args()
    common = dict(inference_s=a.inference, produce_s=a.produce, drain_s=a.drain, deadline_s=a.deadline,
                  image_bytes=a.image_bytes, queue_results=a.queue_results, seed=a.seed, retry_s=a.retry,
                  events=[int(x) for x in a.events.split(",")])
    if a.policy in ("M0", "M1"):
        from mqtt_device import MqttDevice

        device = MqttDevice(a.host, a.port, qos=int(a.policy[1]), keepalive_s=max(1, round(a.idle_timeout)),
                            connect_timeout_s=a.attempt, **common)
        start = device.run(a.start_at)
    else:
        from mission_device import Device

        device = Device(a.policy, a.port, a.cert, host=a.host, idle_timeout=a.idle_timeout, attempt_s=a.attempt,
                        unacked_image=a.unacked_image, **common)
        start = asyncio.run(device.run(TRACKS, a.start_at))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "ledger.jsonl").write_text("".join(json.dumps(r) + "\n" for r in device.ledger))
    (a.out / "device.json").write_text(json.dumps(dict(start=start, stats=device.stats, policy=a.policy), indent=1) + "\n")


if __name__ == "__main__":
    main()
