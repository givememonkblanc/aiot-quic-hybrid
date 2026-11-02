#!/usr/bin/env python3
"""make_slot_template.py for a detector with a count head (model-c3: channels background, 1, 2, 3).

replay_workers.Detector reads channel 1 as the screw score, which is right for f1 (background, screw)
only. Here the score is 1 - background, as the device (threshold 77/255), and each peak carries its
count, argmax over channels 1..3 + 1 (touching screws give one peak of count 2). The slots are those of
make_slot_template.build; the multiplicity of a slot is the median count of the peaks at its tracked
crossings (the observation after the line), and the slot is written that many times, as
make_slot_template_runs.py does.

usage: make_slot_template_c3.py RUN_DIR [--model TFLITE] [--out JSON] [--screws 19]"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

import make_slot_template as M
import replay_workers as R

C3 = Path("/home/ryzen395/data/aiot_quic_results/screw_turntable/model-c3/screw_turntable_int8.tflite")
COUNTS = {}


class CountDetector(R.Detector):
    def __call__(self, path):
        y = self.luma(path)
        hh, ww = y.shape
        rx, ry = ww * R.ROI[0] // 1000, hh * R.ROI[1] // 1000
        rw, rh = ww * R.ROI[2] // 1000 - rx, hh * R.ROI[3] // 1000 - ry
        sy = ry + (np.arange(self.h) * rh) // self.h
        sx = rx + (np.arange(self.w) * rw) // self.w
        crop = y[np.ix_(sy, sx)].astype(np.float32)
        scale, zero = self.inp["quantization"]
        q = np.clip(np.floor(crop / 255.0 / scale + 0.5).astype(np.int32) + zero, -128, 127).astype(np.int8)
        self.it.set_tensor(self.inp["index"], q[None, :, :, None])
        self.it.invoke()
        o = self.it.get_tensor(self.out["index"])[0].astype(np.float32)
        oscale, ozero = self.out["quantization"]
        p = np.clip((o - ozero) * oscale, 0, 1)
        score = np.clip(1.0 - p[..., 0], 0, 1)
        conf = (score.astype(np.float32) * np.float32(255)).astype(np.int32)
        pts = R.peaks(conf)
        gh, gw = conf.shape
        x0, y0 = 256 * R.ROI[0] // 1000, 256 * R.ROI[1] // 1000
        xw, yh = 256 * R.ROI[2] // 1000 - x0, 256 * R.ROI[3] // 1000 - y0
        for x, yy in pts:
            gx = min(gw - 1, max(0, round(((x - x0) * 2 * gw / xw - 1) / 2)))
            gy = min(gh - 1, max(0, round(((yy - y0) * 2 * gh / yh - 1) / 2)))
            COUNTS[(str(path), x, yy)] = int(np.argmax(p[gy, gx, 1:4])) + 1
        return pts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=Path)
    ap.add_argument("--model", type=Path, default=C3)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--screws", type=int, default=19)
    a = ap.parse_args()
    R.Detector = CountDetector                      # make_slot_template.build makes R.Detector(model)
    frames = dict(R.load_trace(a.run))
    t = M.build(a.run, a.model)
    # counts at the crossings: the slot members are not returned by build, so redo the tracking on the
    # same detections (cached in COUNTS) to find, per slot, the counts of the peaks after the line
    det = {}
    for (p, x, y), c in COUNTS.items():
        det.setdefault(int(Path(p).stem), []).append((x, y, c))
    t_of = {s: tt for s, tt in frames.items()}
    by_t = {round(tt, 6): s for s, tt in t_of.items()}
    crossings, _, _ = R.track_truth(sorted(frames.items()), {s: [(x, y) for x, y, _ in det.get(s, [])] for s in frames})
    P, ref = t["period_s"], t["slot0_time_s_device_clock"]
    per = [[] for _ in t["slots"]]
    for c in crossings:
        at, ax, ay = c["after"]
        s = by_t.get(round(at, 6))
        cnt = next((k for x, y, k in det.get(s, []) if x == ax and y == ay), None)
        if cnt is None:
            continue
        ph = (c["t"] - ref) % P
        best = min(range(len(t["slots"])), key=lambda i: (
            min(abs(ph - t["slots"][i]["phase_s"]), P - abs(ph - t["slots"][i]["phase_s"])) / 0.45
            + abs(c["y"] - t["slots"][i]["y"]) / 10))
        sl = t["slots"][best]
        if min(abs(ph - sl["phase_s"]), P - abs(ph - sl["phase_s"])) <= 0.45 and abs(c["y"] - sl["y"]) <= 10:
            per[best].append(cnt)
    out = []
    for sl, cs in zip(t["slots"], per):
        m = int(np.median(cs)) if cs else 1
        for _ in range(m):
            out.append(dict(sl, slot=len(out), multiplicity=m, counts=dict(zip(*np.unique(cs, return_counts=True)))
                            if cs else {}))
    for s in out:
        s["counts"] = {int(k): int(v) for k, v in s["counts"].items()}
    t.update(places=len(t["slots"]), screws=len(out), slots=out,
             source=t["source"] + "; count head: multiplicity = median count at the crossings (make_slot_template_c3.py)")
    dest = a.out or a.run / "slot_template.json"
    dest.write_text(json.dumps(t, indent=1) + "\n")
    print(f"{dest}: {t['places']} places, {t['screws']} screws, period {t['period_s']} s, "
          f"{t['trace']['rotations']} rotations, {t['trace']['tracked_crossings']} tracked crossings over "
          f"{t['trace']['span_s']} s, {t['trace']['frames']} frames; multiplicities "
          f"{[s['multiplicity'] for s in t['slots'] if s['multiplicity'] > 1]}; seen {[s['seen'] for s in t['slots']]}")
    if t["screws"] != a.screws:
        print(f"WARNING: {t['screws']} screws, expected {a.screws}", file=sys.stderr)
    return 0 if t["screws"] == a.screws else 2


if __name__ == "__main__":
    sys.exit(main())
