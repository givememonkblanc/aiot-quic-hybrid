#!/usr/bin/env python3
"""Live fuser on the laptop: the passages of the turntable from the detections of the camera ESP32
and of the helper ESP32, both received over MoQ, counted once each and emitted in the order the
screws passed.

It follows the event log of a subscriber of the relay (sub-<name>/events.jsonl, written as Objects
arrive):
  camera  "object" records of ai/events: properties["120"] = capture time (ms, camera clock),
          result.boxes = detections (x, y in model coordinates)
  helper  records with a "helper" field (helper/events): helper.capture_ms (camera clock, from the
          offloaded frame), helper.dets = [[x, y], ...]
A detection list is a sample at its capture time. Samples are counted in capture order by the
counter of the firmware (passage_assoc.py, ASSOC=1). A sample is held until a later one has been
seen whose capture time is WAIT ms newer (the delay of the helper): then every older sample is
counted and its passages are emitted. A sample older than the last counted one arrives too late to
keep the order; it is dropped and counted as late.

Output (JSON lines): {"t_out": host time, "cap_ms": capture time, "id": passage number, "x", "y",
"source": "camera"|"helper"}; at the end a summary line {"summary": ...}.

usage: live_fuser.py --events results/<run>/sub-qa/events.jsonl --out results/<run>/fused.jsonl
                     [--wait-ms 1000] [--seconds 480]"""
import argparse
import json
import time
from pathlib import Path

from passage_assoc import Counter


def sample(record):
    if record.get("event") not in ("object", "fetch_object"):
        return None
    if "helper" in record:
        h = record["helper"]
        if h.get("capture_ms"):
            return int(h["capture_ms"]), [tuple(p[:2]) for p in h.get("dets", [])], "helper"
        return None
    res = record.get("result")
    cap = record.get("properties", {}).get("120")
    if res and cap is not None:
        return int(cap), [(b["x"], b["y"]) for b in res.get("boxes", [])], "camera"
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--events", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--wait-ms", type=int, default=1000)
    p.add_argument("--seconds", type=float, default=480)
    a = p.parse_args()
    counter, pending, done_cap = Counter(), {}, -1
    stats = dict(camera=0, helper=0, duplicate=0, late=0, passages=0)
    end = time.time() + a.seconds
    out = open(a.out, "a", buffering=1)
    while not a.events.exists() and time.time() < end:
        time.sleep(0.2)
    f = open(a.events) if a.events.exists() else None
    newest = -1

    def flush(limit):
        nonlocal done_cap
        for cap in sorted(c for c in pending if c <= limit):
            pts, src = pending.pop(cap)
            for pid, (x, y) in counter.update(pts, cap):
                stats["passages"] += 1
                out.write(json.dumps(dict(t_out=round(time.time(), 3), cap_ms=cap, id=pid, x=x, y=y, source=src)) + "\n")
            done_cap = cap

    while f and time.time() < end:
        line = f.readline()
        if not line:
            time.sleep(0.05)
            continue
        try:
            s = sample(json.loads(line))
        except json.JSONDecodeError:
            continue
        if not s:
            continue
        cap, pts, src = s
        if cap <= done_cap:
            stats["late"] += 1
            continue
        if cap in pending:
            stats["duplicate"] += 1
            continue
        pending[cap] = (pts, src)
        stats[src] += 1
        newest = max(newest, cap)
        flush(newest - a.wait_ms)
    flush(float("inf"))
    out.write(json.dumps(dict(summary=stats)) + "\n")


if __name__ == "__main__":
    main()
