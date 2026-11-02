#!/usr/bin/env python3
"""Reference count of a turntable run from the full-resolution preview frames.

The preview frames arrive about every 0.6 s, against 0.45 s or more between two results of
the device, and are taken at four times the resolution that the device analyses. The
objects are detected with dark_blobs.py at 400x300, associated from frame to frame by the
nearest neighbour within a gate, and a passage is counted when a track crosses the
counting line in the configured direction. The device count over the same interval is
read from its console copy.

usage: turntable_truth.py RUN [--line 128] [--axis 0] [--direction 1]"""

import argparse
import glob
import json
import re
from pathlib import Path

import numpy as np

import dark_blobs


def frames(run):
    t = {}
    for line in (run / "events.jsonl").read_text().splitlines():
        e = json.loads(line)
        if e.get("event") == "object" and e.get("alias") == 1:
            t[e["group"]] = e["t"]
    out = []
    for f in sorted(glob.glob(str(run / "frame-*.jpg"))):
        seq = int(re.search(r"frame-(\d+)", f).group(1))
        if seq in t:
            out.append((seq, t[seq], f))
    return out


def track(detections, gate):
    """detections: [(seq, [(x, y)])]. Returns tracks as lists of (seq, x, y)."""
    live, done = [], []
    for seq, points in detections:
        used = set()
        for tr in sorted(live, key=lambda tr: -len(tr)):
            _, x, y = tr[-1]
            best, bd = None, gate
            for i, (px, py) in enumerate(points):
                d = ((px - x) ** 2 + (py - y) ** 2) ** 0.5
                if i not in used and d < bd:
                    best, bd = i, d
            if best is not None:
                used.add(best)
                tr.append((seq, *points[best]))
        for tr in [tr for tr in live if tr[-1][0] != seq]:
            live.remove(tr)
            done.append(tr)
        live += [[(seq, *p)] for i, p in enumerate(points) if i not in used]
    return done + live


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("run", type=Path)
    p.add_argument("--line", type=float, default=128)
    p.add_argument("--axis", type=int, default=0)
    p.add_argument("--direction", type=int, default=1)
    p.add_argument("--gate", type=float, default=14, help="largest movement between two frames, 0..255 units")
    a = p.parse_args()
    dark_blobs.WIDTH, dark_blobs.HEIGHT = 400, 300
    fr = frames(a.run)
    det = []
    for seq, _, f in fr:
        blobs = dark_blobs.detect(dark_blobs.luma(f), area_min=8, area_max=320)
        det.append((seq, [(x, y) for x, y, *_ in blobs]))
    tracks = track(det, a.gate)
    crossings = []
    for tr in tracks:
        for (s0, *p0), (s1, *p1) in zip(tr, tr[1:]):
            before = (p0[a.axis] - a.line) * a.direction
            after = (p1[a.axis] - a.line) * a.direction
            if before < 0 <= after:
                crossings.append(s1)
    first, last = fr[0][0], fr[-1][0]
    device = []
    for line in (a.run / "serial.log").read_text(errors="replace").splitlines():
        if "[MOQ_PASSAGE]" in line:
            e = json.loads(line.split("[MOQ_PASSAGE] ", 1)[1])
            device.append(e["seq"])
    in_window = [s for s in device if first <= s <= last]
    report = dict(run=a.run.name, frames=len(fr), seq_first=first, seq_last=last, tracks=len(tracks),
                  reference=len(crossings), device=len(in_window), device_total=len(device),
                  mean_objects=float(np.mean([len(d) for _, d in det])))
    print(json.dumps(report))
    (a.run / "turntable-truth.json").write_text(json.dumps(dict(report, crossings=crossings, device_passages=device)) + "\n")


if __name__ == "__main__":
    main()
