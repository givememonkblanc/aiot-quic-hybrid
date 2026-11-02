#!/usr/bin/env python3
"""Live fuser on the laptop, ordered: live_fuser.py with the ordered output stage of replay_fuser.py.

As live_fuser, it follows the event log of a subscriber of the relay (camera ai/events and helper
helper/events, both over MoQ), holds a sample until one WAIT ms newer has been seen, and counts the
samples in capture order with the firmware's counter. In addition:
- the crossing time of each passage is estimated with the speed of its radius, v(y), fitted online
  to the tracks (KeyedCounter speed="fit"), not with the track's own or the shared speed;
- the counted passages are released in the order of their crossing times, once the newest counted
  capture time is H ms past the crossing (H = 0: the passages counted together are sorted);
- with --speed fit-assoc, the association predicts every track with v(y) as well (rigid rotation).
Replayed on the off1 runs (replay_fuser.py, eval_offload.py), WAIT 1000 gives the same passages as
WAIT 2500; H 0 leaves inversions from screws first detected 1.2-1.7 s past their crossing (under
the 1 Mbit/s uplink), H 1000 none beyond the resolution of the ground truth, for 1 s more delay.

Output (JSON lines): {"t_out", "cap_ms", "crossing_ms", "id", "x", "y", "source"}; at the end a
summary line {"summary": ...}.

usage: live_fuser2.py --events results/<run>/sub-qa/events.jsonl --out FILE [--wait-ms 1000]
                      [--horizon-ms 1000] [--speed fit|fit-assoc] [--seconds 480]"""
import argparse
import json
import time
from pathlib import Path

from live_fuser import sample
from replay_fuser import KeyedCounter


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--events", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--wait-ms", type=int, default=1000)
    p.add_argument("--horizon-ms", type=int, default=1000)
    p.add_argument("--speed", choices=("track", "fit", "fit-assoc"), default="fit")
    p.add_argument("--seconds", type=float, default=480)
    a = p.parse_args()
    counter, pending, done_cap, buf = KeyedCounter(a.speed), {}, -1, []
    stats = dict(camera=0, helper=0, duplicate=0, late=0, passages=0)
    end = time.time() + a.seconds
    out = open(a.out, "a", buffering=1)
    while not a.events.exists() and time.time() < end:
        time.sleep(0.2)
    f = open(a.events) if a.events.exists() else None
    newest = -1

    def release(limit_ms):
        nonlocal buf
        buf.sort(key=lambda e: e[0])
        keep = []
        for e in buf:
            if e[0] <= limit_ms:
                stats["passages"] += 1
                out.write(json.dumps(dict(t_out=round(time.time(), 3), cap_ms=e[1], crossing_ms=e[0], id=stats["passages"],
                                          x=e[2][0], y=e[2][1], source=e[3])) + "\n")
            else:
                keep.append(e)
        buf = keep

    def flush(limit):
        nonlocal done_cap
        for cap in sorted(c for c in pending if c <= limit):
            pts, src = pending.pop(cap)
            for k, (_, pt) in enumerate(counter.update(pts, cap)):
                buf.append((cap - counter._crossed[k], cap, pt, src))
            done_cap = cap
        if done_cap >= 0:
            release(done_cap - a.horizon_ms)

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
    release(float("inf"))
    out.write(json.dumps(dict(summary=stats)) + "\n")


if __name__ == "__main__":
    main()
