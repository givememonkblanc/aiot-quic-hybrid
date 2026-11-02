#!/usr/bin/env python3
"""Offload runs: the passages counted from the detections of the camera alone and from the camera
and the helper ESP32 together, with the same counter (passage_assoc.py, the reference of the
firmware's ASSOC=1 counter), in capture order.

Inputs of a run directory:
  serial.log         camera: [MOQ_TRACK] {"seq","cap_ms","pts":[[x,y],...]} per local inference,
                     [MOQ_OFFLOAD] {"seq","capture_us","bytes","result"} per offered frame
  helper.log         helper: [HELPER_DET] {"group","capture_ms","dets":[[x,y],...],...} per inference
                     (capture_ms on the camera clock: property 0x78 of the offloaded Object)
Both detectors see the same frames in the same coordinates, so a helper detection list is an
inference of the camera at another capture time. A frame inferred by both is taken once.

Output per run: local inferences, offloaded frames (offered, accepted), helper inferences, passages
(camera alone, fused), passages per rotation (19 screws in scene 2; the rotation period from the
passage train), and the recall estimate passages / (19 x rotations).

usage: fuse_offload.py results/<run> [results/<run> ...]"""
import json
import sys
from pathlib import Path

import numpy as np

from passage_assoc import Counter

SCREWS = 19


def tagged(path, tag):
    out = []
    if not path.exists():
        return out
    for line in path.read_text(errors="replace").splitlines():
        i = line.find(f"[{tag}] ")
        if i >= 0:
            try:
                out.append(json.loads(line[i + len(tag) + 3:]))
            except json.JSONDecodeError:
                pass
    return out


def count(samples):
    c = Counter()
    events = []
    for cap_ms, pts in sorted(samples, key=lambda s: s[0]):
        for e in c.update([tuple(p) for p in pts][:24], cap_ms):
            events.append(cap_ms)
    return events


def period(times):
    t = np.array(sorted(times), float) / 1000
    if len(t) < 20:
        return None
    x = np.zeros(int((t[-1] - t[0]) * 10) + 1)
    x[((t - t[0]) * 10).astype(int)] = 1
    x = np.convolve(x, np.ones(5), "same")
    ac = [np.dot(x[:-k], x[k:]) for k in range(100, min(1500, len(x) // 2))]
    return (np.argmax(ac) + 100) / 10 if ac else None


def main():
    for d in map(Path, sys.argv[1:]):
        local = [(r["cap_ms"], r["pts"]) for r in tagged(d / "serial.log", "MOQ_TRACK")]
        offered = tagged(d / "serial.log", "MOQ_OFFLOAD")
        helper = [(r["capture_ms"], r["dets"]) for r in tagged(d / "helper.log", "HELPER_DET") if r.get("capture_ms")]
        seen = {c for c, _ in local}
        fused = local + [h for h in helper if h[0] not in seen]
        ev_local, ev_fused = count(local), count(fused)
        P = period(ev_fused) or period(ev_local)
        span = (max(c for c, _ in fused) - min(c for c, _ in fused)) / 1000 if fused else 0
        rot = span / P if P else None
        row = dict(run=d.name, local_inferences=len(local), offered=len(offered),
                   accepted=sum(1 for o in offered if o.get("result") == 0), helper_inferences=len(helper),
                   passages_local=len(ev_local), passages_fused=len(ev_fused), period_s=P,
                   recall_local=len(ev_local) / (SCREWS * rot) if rot else None,
                   recall_fused=len(ev_fused) / (SCREWS * rot) if rot else None)
        print(json.dumps(row))


if __name__ == "__main__":
    main()
