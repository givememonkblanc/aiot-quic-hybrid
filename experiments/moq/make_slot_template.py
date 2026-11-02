#!/usr/bin/env python3
"""Slot template of a turntable scene, for analyze_rotations.py.

From a frame-trace run (RUN_DIR/trace/frames/<seq>.jpg and trace/index.csv): the host detector of the
device (replay_workers.Detector) on every trace frame, frame-to-frame tracks, their crossings of the
counting line, grouped by rotation phase into slots (replay_workers.track_truth, periodic_truth).
For each slot: crossing phase relative to slot 0 (the first screw after the longest gap of the
rotation), y at the line, velocity (vx, vy) near the line (straight-line fit over the track's
positions with 100 <= x <= 160), and how many rotations saw it.

usage: make_slot_template.py RUN_DIR [--out JSON] [--screws 19]
       (default output RUN_DIR/slot_template.json; system python3)"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

import replay_workers as R


def build(run, model=R.MODEL):
    frames = R.load_trace(run)
    if len(frames) < 50:
        raise SystemExit(f"{run}: only {len(frames)} trace frames")
    D = R.Detector(model)
    det = {s: D(run / "trace" / "frames" / f"{s:07d}.jpg") for s, _ in frames}
    t0, t1 = frames[0][1], frames[-1][1]
    crossings, tracks, _ = R.track_truth(frames, det)
    truth, P, slots, ref = R.periodic_truth(crossings, t0, t1, R.period_estimate([c["t"] for c in crossings]))
    by_phase = sorted(range(len(slots)), key=lambda i: slots[i]["phi"])
    gaps = [(slots[by_phase[(k + 1) % len(by_phase)]]["phi"] - slots[by_phase[k]]["phi"]) % P
            for k in range(len(by_phase))]
    start = (int(np.argmax(gaps)) + 1) % len(by_phase)
    order = by_phase[start:] + by_phase[:start]
    byid = {t["id"]: t for t in tracks}
    out = []
    for rank, i in enumerate(order):
        sl = slots[i]
        vx, vy = [], []
        for c in sl["members"].values():
            o = [q for q in byid[c["track"]]["obs"] if 100 <= q[1] <= 160]
            if len(o) >= 3:
                tt = np.array([q[0] for q in o])
                vx.append(np.polyfit(tt, [q[1] for q in o], 1)[0])
                vy.append(np.polyfit(tt, [q[2] for q in o], 1)[0])
        out.append(dict(slot=rank, phase_s=round((sl["phi"] - slots[order[0]]["phi"]) % P, 3), y=round(sl["y"], 1),
                        vx=round(float(np.median(vx)), 1) if vx else None,
                        vy=round(float(np.median(vy)), 1) if vy else None, seen=len(sl["members"]),
                        phase_sd_s=round(float(np.std([c["t"] - k * P for k, c in sl["members"].items()])), 3),
                        y_sd=round(float(np.std([c["y"] for c in sl["members"].values()])), 1)))
    rotations = int((t1 - ref) // P) + 1
    return dict(source=f"{run.name} all-frame host detections ({Path(model).parent.name}), tracked crossings grouped by rotation phase",
                screws=len(out), period_s=round(P, 4), line=R.LINE, axis="x",
                slot0_time_s_device_clock=round(ref + slots[order[0]]["phi"], 3),
                cell=dict(x=round(256 * 0.4 / 20, 2), y=round(256 * 0.4 / 15, 2)),
                trace=dict(frames=len(frames), span_s=round(t1 - t0, 1), rotations=rotations,
                           tracked_crossings=len(crossings)),
                slots=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--screws", type=int, default=19)
    a = ap.parse_args()
    t = build(a.run)
    out = a.out or a.run / "slot_template.json"
    out.write_text(json.dumps(t, indent=1) + "\n")
    print(f"{out}: {t['screws']} slots, period {t['period_s']} s, {t['trace']['rotations']} rotations, "
          f"{t['trace']['tracked_crossings']} tracked crossings over {t['trace']['span_s']} s")
    missing_v = [s["slot"] for s in t["slots"] if s["vx"] is None]
    if t["screws"] != a.screws:
        print(f"WARNING: {t['screws']} slots found, expected {a.screws} (merged or hidden screws, or too few rotations)",
              file=sys.stderr)
    if missing_v:
        print(f"WARNING: no velocity for slots {missing_v}", file=sys.stderr)
    if t["trace"]["rotations"] < 4:
        print(f"WARNING: only {t['trace']['rotations']} rotations in the trace; slots need >= half of them", file=sys.stderr)
    return 0 if t["screws"] == a.screws else 2


if __name__ == "__main__":
    sys.exit(main())
