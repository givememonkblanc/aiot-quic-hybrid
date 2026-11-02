#!/usr/bin/env python3
"""The frames the camera drops, inferred by an idle helper: offline, from a trace run, judged at the laptop.

The camera captures ~6 frames/s and infers ~1/s (601 ms of model, capture and encode besides); the other
frames are dropped. A helper ESP32 (10/6: inference p50 0.48 s, report 0.77 s after publish) can infer
some of them. Here, on a trace run (serial.log [MOQ_TRACK]: the samples the camera really inferred, with
its detections; trace/frames: the captured frames that reached the host):

- camera samples: the [MOQ_TRACK] records (capture time, detections), arriving CAM_LAT after capture;
- helper samples: whenever the helper is free, the newest captured frame not inferred by the camera
  (of those in the trace; a frame reaches the helper OFFLOAD_LAT after capture), inferred by the host
  model of the camera (make_slot_template_c3.CountDetector, count copies as the device emits them) in
  HELPER_MS, arriving HELPER_LAT after its inference; --helpers N helpers in parallel;
- the laptop: replay_fuser.replay of both (camera and helper records as the subscriber logs them),
  judged on the slot template.

usage: offload_sim.py RUN [--template T] [--helpers 0 1 2] [--helper-ms 480] [--wait-ms 2500] [--horizon-ms 1000]
                      [--max-step 48] [--max-missed 2]"""
import argparse
import csv
import json
import tempfile
from pathlib import Path

import numpy as np

import analyze_rotations as A
import make_slot_template_c3 as C
import replay_fuser as R

CAM_LAT = 0.7
OFFLOAD_LAT = 0.25
HELPER_LAT = 0.3
KEYS = ("n", "complete_rotations", "slot_recall", "missing", "duplicate", "false", "inversions")


def camera_samples(run):
    out = []
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        if "[MOQ_TRACK] " in line:
            try:
                r = json.loads(line[line.index("{"):])
            except ValueError:
                continue
            out.append((r["seq"], r["cap_ms"], [tuple(p) for p in r["pts"]]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=Path)
    ap.add_argument("--template", type=Path)
    ap.add_argument("--helpers", type=int, nargs="+", default=[0, 1, 2])
    ap.add_argument("--helper-ms", type=float, default=480)
    ap.add_argument("--wait-ms", type=int, default=2500)
    ap.add_argument("--horizon-ms", type=int, default=1000)
    ap.add_argument("--max-step", type=int, default=48, help="gate of the laptop counter (passage_assoc.Counter)")
    ap.add_argument("--max-missed", type=int, default=2)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    base = R.KeyedCounter

    class Gated(base):
        def __init__(self, *args, **kw):
            super().__init__(*args, max_step=a.max_step, max_missed=a.max_missed, **kw)
    R.KeyedCounter = Gated
    tp = A.Template(a.template or a.run / "slot_template.json")
    cam = camera_samples(a.run)
    cam_seq = {s for s, _, _ in cam}
    have = {int(p.stem) for p in (a.run / "trace" / "frames").glob("*.jpg")}
    idx = sorted((int(r[1]) / 1000, int(r[0])) for r in csv.reader(open(a.run / "trace" / "index.csv")) if int(r[0]) in have)
    D = C.CountDetector(C.C3)
    cache = {}

    def detect(seq):
        if seq not in cache:
            path = a.run / "trace" / "frames" / f"{seq:07d}.jpg"
            pts = D(path)
            cache[seq] = [p for p in pts for _ in range(C.COUNTS.get((str(path), *p), 1))]
        return cache[seq]

    t0, t1 = cam[0][1], cam[-1][1]
    results = []
    for nh in a.helpers:
        recs = [dict(t=cap / 1000 + CAM_LAT, event="object", properties={"120": cap},
                     result=dict(boxes=[dict(x=x, y=y) for x, y in pts])) for _, cap, pts in cam]
        free = [t0] * nh
        used = set()
        caps = np.array([c for c, _ in idx])
        while nh:
            h = int(np.argmin(free))
            now = free[h]
            if now > t1:
                break
            # newest frame captured by now - OFFLOAD_LAT, not inferred by the camera nor a helper
            k = int(np.searchsorted(caps, now - OFFLOAD_LAT * 1000, side="right")) - 1
            while k >= 0 and (idx[k][1] in cam_seq or idx[k][1] in used):
                k -= 1
            if k < 0 or caps[k] < now - 1500:
                free[h] = now + 50
                continue
            cap, seq = idx[k]
            used.add(seq)
            done = now + a.helper_ms
            recs.append(dict(t=(done / 1000) + HELPER_LAT, event="object",
                             helper=dict(capture_ms=int(cap), dets=[list(p) for p in detect(seq)])))
            free[h] = done
        recs.sort(key=lambda r: r["t"])
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write("".join(json.dumps(r) + "\n" for r in recs))
        fused, st, delay, _ = R.replay(Path(f.name), a.wait_ms, a.horizon_ms, "fit-assoc", True)
        Path(f.name).unlink()
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write("".join(json.dumps(o) + "\n" for o in fused))
        rows, span = A.load(f.name)
        Path(f.name).unlink()
        r = A.analyze(rows, span, tp, 0.16)
        from collections import Counter
        out = dict(helpers=nh, camera_samples=st["camera"], helper_samples=st["helper"],
                   samples_per_s=round((st["camera"] + st["helper"]) / ((t1 - t0) / 1000), 2),
                   **{k: r[k] for k in KEYS}, missing_by_slot=dict(Counter(j for o in r["rotations"] for j in o["missing"])),
                   delay_p50_s=round(float(np.median(delay)), 2) if delay else None,
                   delay_p95_s=round(float(np.percentile(delay, 95)), 2) if delay else None)
        results.append(out)
        print(json.dumps(out), flush=True)
    if a.json:
        a.json.write_text(json.dumps(results, indent=1) + "\n")


if __name__ == "__main__":
    main()
