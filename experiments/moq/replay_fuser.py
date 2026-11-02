#!/usr/bin/env python3
"""Offline replay of the laptop fuser (live_fuser.py) on the event log of a run, at the recorded
arrival times, with an ordered output stage.

live_fuser emits the passages of a sample as soon as it counts it, in the order of crossing within
that sample only. A screw detected late (first seen well past the line, in a later sample than a
screw that crossed after it) then arrives after it: an inversion. The ordered stage keeps the
counted passages in a buffer by estimated crossing time (capture time minus the travel past the line
over the track's speed, the key of the counter) and releases a passage once the newest counted
capture time is H later than its crossing: any screw that crossed earlier and is detected at most H
after its crossing is then already in the buffer. H trades order against delay; H = 0 releases at
once (live_fuser's order, but sorted across samples counted together).

For each run and H: the fused stream (replay/fused-<speed>-H<ms>.jsonl, the format of live_fuser) and its delay:
release time minus crossing time on the host clock (camera clock mapped by the smallest arrival
minus capture time of the camera's samples, so the delay is above the fastest path), and the
detection lag (newest capture at counting minus crossing) that sets the H needed.

usage: replay_fuser.py results/<run> [...] [--wait-ms 2500] [--horizons 0,500,1000,1500,2000,3000]
                       [--speed track|fit|fit-assoc]"""
import argparse
import json
from pathlib import Path

import numpy as np

from live_fuser import sample
from passage_assoc import Counter, cdiv


class KeyedCounter(Counter):
    """Counter whose events carry their estimated crossing time (capture ms - key).

    speed="track": the key of the counter (travel past the line over the track's speed, or the
    shared flow for a new track). speed="fit": over the speed at the line predicted from the lateral
    position, v(y) = a + b y, fitted online (least squares, then again without residuals beyond 3
    MAD) to the speeds of the tracks seen moving in the last WINDOW updates. On a turntable the speed
    along the axis is proportional to the radius, so it is linear in y at the line; a new track or a
    track with few observations then gets the speed of its radius, not a noisy or shared one.
    speed="fit-assoc": the same, and the prediction of the association uses v(y) too (rigid
    rotation: every track moves along the axis at the speed of its radius), instead of the track's
    own speed averaged over its last results."""

    WINDOW = 400

    def __init__(self, speed="track", **kw):
        super().__init__(**kw)
        self.speed, self.obs, self.coef = speed, [], None

    def cost(self, t, p, now):
        if self.speed == "fit-assoc" and self.coef is not None:
            v = self.coef[0] + self.coef[1] * self.lat(t["x"], t["y"])
            if v > 0:
                t = dict(t, va=int(v), vl=t["vl"] if t["moving"] else self.fl, moving=True)
        return super().cost(t, p, now)

    def count(self, t, p, events):
        super().count(t, p, events)
        if self.speed in ("fit", "fit-assoc") and self.coef is not None:
            v = self.coef[0] + self.coef[1] * self.lat(*p)
            if v > 0:
                self._keys[-1] = cdiv(abs(self.ax(*p) - self.line) * 16000, int(v))

    def update(self, points, now_ms):
        out = super().update(points, now_ms)
        for t in self.tracks:
            if t is not None and t["moving"] and t["ms"] == int(now_ms) and t["va"] > 0:
                self.obs.append((self.lat(t["x"], t["y"]), t["va"]))
        self.obs = self.obs[-self.WINDOW:]
        if len(self.obs) >= 30:
            y, v = np.array(self.obs, float).T
            b, a = np.polyfit(y, v, 1)
            r = v - (a + b * y)
            keep = np.abs(r) <= 3 * max(np.median(np.abs(r - np.median(r))), 1.0)
            b, a = np.polyfit(y[keep], v[keep], 1)
            self.coef = (a, b)
        return out

    def ordered(self, events):
        order = sorted(range(len(events)), key=lambda i: -self._keys[i])   # stable, all events
        base = events[0][0] if events else 0
        self._crossed = [self._keys[i] for i in order]
        return [(base + k, events[i][1]) for k, i in enumerate(order)]


def replay(events_path, wait_ms, horizon_ms, speed="track", confirm=False):
    recs = []
    for line in open(events_path):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        s = sample(r)
        if s:
            recs.append((r["t"], *s))
    counter = KeyedCounter(speed, confirm=confirm)
    pending, done_cap, newest, buf, out = {}, -1, -1, [], []
    stats = dict(camera=0, helper=0, duplicate=0, late=0)
    lag = []

    def release(now_t, limit_ms):
        nonlocal buf
        buf.sort(key=lambda e: e[0])
        keep = []
        for e in buf:
            if e[0] <= limit_ms:
                out.append(dict(t_out=now_t, cap_ms=e[1], crossing_ms=e[0], id=len(out) + 1, x=e[2][0], y=e[2][1], source=e[3]))
            else:
                keep.append(e)
        buf = keep

    def flush(now_t, limit):
        nonlocal done_cap
        for cap in sorted(c for c in pending if c <= limit):
            pts, src = pending.pop(cap)
            for k, (_, p) in enumerate(counter.update(pts, cap)):
                tc = cap - counter._crossed[k]
                buf.append((tc, cap, p, src))
                lag.append(cap - tc)
            done_cap = cap
        if done_cap >= 0:
            release(now_t, done_cap - horizon_ms)

    for t, cap, pts, src in recs:
        if cap <= done_cap:
            stats["late"] += 1
            continue
        if cap in pending:
            stats["duplicate"] += 1
            continue
        pending[cap] = (pts, src)
        stats[src] += 1
        newest = max(newest, cap)
        flush(t, newest - wait_ms)
    end_t = recs[-1][0] if recs else 0
    flush(end_t, float("inf"))
    release(end_t, float("inf"))
    cam = [t - cap / 1000 for t, cap, _, src in recs if src == "camera"]
    offset = min(cam) if cam else 0
    delay = [o["t_out"] - (o["crossing_ms"] / 1000 + offset) for o in out]
    return out, stats, delay, lag


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--wait-ms", type=int, default=2500)
    ap.add_argument("--horizons", default="0,500,1000,1500,2000,3000")
    ap.add_argument("--speed", choices=("track", "fit", "fit-assoc"), default="track")
    a = ap.parse_args()
    for run in a.runs:
        ev = run / "sub-qa" / "events.jsonl"
        (run / "replay").mkdir(exist_ok=True)
        for h in map(int, a.horizons.split(",")):
            out, stats, delay, lag = replay(ev, a.wait_ms, h, a.speed)
            path = run / "replay" / f"fused-{a.speed}-H{h}.jsonl"
            path.write_text("".join(json.dumps(o) + "\n" for o in out))
            d = np.array(delay) if delay else np.zeros(1)
            print(json.dumps(dict(run=run.name, speed=a.speed, H_ms=h, passages=len(out), **stats,
                                  delay_p50_s=round(float(np.median(d)), 2), delay_p95_s=round(float(np.percentile(d, 95)), 2),
                                  lag_p95_ms=int(np.percentile(lag, 95)) if lag else None, lag_max_ms=max(lag) if lag else None)))


if __name__ == "__main__":
    main()
