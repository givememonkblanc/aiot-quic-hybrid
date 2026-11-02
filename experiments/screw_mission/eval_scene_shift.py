#!/usr/bin/env python3
"""Object-level evaluation of the device screw detector on labelled frames of a scene, with the
failures sorted by cause.

Truth: label_turntable.py labels of full-resolution frames (screw centroids, touching screws split).
Detector: the device pipeline replayed on the host (moq/replay_workers.Detector: 1/8 DCT decode,
RGB565 luma, the ROI, int8, FOMO peaks over the device threshold). Both in result units (0..255 of
the frame). Only truth inside the ROI is scored; a detection and a screw match when they are within
MATCH cells (Chebyshev, in cells of the 20x15 grid), one to one (Hungarian).

Causes:
- a missed screw is "touching" when another screw is within TOUCH cells, "border" when it is within
  one cell of the ROI edge, else "isolated";
- a false detection is "border" within one cell of the ROI edge, "off-plate" outside the plate
  (label_turntable.plate_hull), "near-screw" within TOUCH cells of a screw (a second peak on one
  screw or a pair), else "plate".

usage: eval_scene_shift.py LABELS.jsonl [...] [--model M.tflite] [--json OUT]"""
import argparse
import json
import sys
from collections import Counter
from pathlib import Path

import cv2
import numpy as np
from scipy.optimize import linear_sum_assignment

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "moq"))
import replay_workers as RW  # noqa: E402
from label_turntable import WIDTH, HEIGHT, plate_hull  # noqa: E402

X0, Y0 = 256 * RW.ROI[0] / 1000, 256 * RW.ROI[1] / 1000
X1, Y1 = 256 * RW.ROI[2] / 1000, 256 * RW.ROI[3] / 1000
MATCH, TOUCH = 1.5, 2.0


class CountDetector(RW.Detector):
    """RW.Detector for a model with a count head (train_turntable_fomo2.py): channels background, 1, 2,
    3 screws; peaks of 1 - background, each emitted as many times as the most likely count."""

    def __call__(self, path):
        y = self.luma(path)
        hh, ww = y.shape
        rx, ry = ww * RW.ROI[0] // 1000, hh * RW.ROI[1] // 1000
        rw, rh = ww * RW.ROI[2] // 1000 - rx, hh * RW.ROI[3] // 1000 - ry
        sy = ry + (np.arange(self.h) * rh) // self.h
        sx = rx + (np.arange(self.w) * rw) // self.w
        crop = y[np.ix_(sy, sx)].astype(np.float32)
        scale, zero = self.inp["quantization"]
        q = np.clip(np.floor(crop / 255.0 / scale + 0.5).astype(np.int32) + zero, -128, 127).astype(np.int8)
        self.it.set_tensor(self.inp["index"], q[None, :, :, None])
        self.it.invoke()
        o = self.it.get_tensor(self.out["index"])[0].astype(np.float32)
        oscale, ozero = self.out["quantization"]
        prob = np.clip((o - ozero) * oscale, 0, 1)
        conf = ((1 - prob[..., 0]) * np.float32(255)).astype(np.int32)
        count = 1 + np.argmax(prob[..., 1:], axis=-1)
        gh, gw = conf.shape
        out = []
        for x, yy in RW.peaks(conf):
            gx = int((x - X0) * gw // (X1 - X0))
            gy = int((yy - Y0) * gh // (Y1 - Y0))
            out += [(x, yy)] * int(count[min(gy, gh - 1), min(gx, gw - 1)])
        return out


def detector(model):
    d = RW.Detector(model)
    return CountDetector(model) if d.out["shape"][-1] > 2 else d


def evaluate(labels, det):
    cx, cy = (X1 - X0) / 20, (Y1 - Y0) / 15
    tot = Counter()
    for line in open(labels):
        r = json.loads(line)
        if r.get("uncertain"):
            continue
        gt = np.array([(o["cx"] * 256, o["cy"] * 256) for o in r["objects"]], float).reshape(-1, 2)
        pr = np.array(det(r["frame"]), float).reshape(-1, 2)
        inside = (gt[:, 0] >= X0) & (gt[:, 0] < X1) & (gt[:, 1] >= Y0) & (gt[:, 1] < Y1)
        g = gt[inside]
        d = np.maximum(np.abs(g[:, None, 0] - pr[None, :, 0]) / cx, np.abs(g[:, None, 1] - pr[None, :, 1]) / cy) \
            if len(g) and len(pr) else np.zeros((len(g), len(pr)))
        C = np.where(d <= MATCH, d, 1e6)
        rr, cc = linear_sum_assignment(C) if C.size else ([], [])
        mg = {i for i, j in zip(rr, cc) if C[i, j] < 1e6}
        mp = {j for i, j in zip(rr, cc) if C[i, j] < 1e6}
        tot["frames"] += 1
        tot["screws"] += len(g)
        tot["hit"] += len(mg)
        tot["detections"] += len(pr)
        edge = lambda p: min((p[0] - X0) / cx, (X1 - p[0]) / cx, (p[1] - Y0) / cy, (Y1 - p[1]) / cy) < 1
        near = lambda p, pts, self_i=None: any(
            k != self_i and max(abs(p[0] - q[0]) / cx, abs(p[1] - q[1]) / cy) <= TOUCH for k, q in enumerate(pts))
        for i, p in enumerate(g):
            if i in mg:
                continue
            tot["miss:" + ("touching" if near(p, gt, np.where(inside)[0][i]) else "border" if edge(p) else "isolated")] += 1
        hull = None
        for j, p in enumerate(pr):
            if j in mp:
                continue
            if edge(p):
                why = "border"
            else:
                if hull is None:
                    gray = cv2.resize(cv2.imread(r["frame"], cv2.IMREAD_GRAYSCALE), (WIDTH, HEIGHT), interpolation=cv2.INTER_AREA)
                    hull = plate_hull(gray)
                hx, hy = int(p[0] / 256 * WIDTH), int(p[1] / 256 * HEIGHT)
                if hull is not None and not hull[min(hy, HEIGHT - 1), min(hx, WIDTH - 1)]:
                    why = "off-plate"
                elif near(p, gt):
                    why = "near-screw"
                else:
                    why = "plate"
            tot["false:" + why] += 1
    tot["recall"] = round(tot["hit"] / max(tot["screws"], 1), 3)
    tot["precision"] = round(tot["hit"] / max(tot["detections"], 1), 3)
    return dict(tot)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("labels", nargs="+", type=Path)
    ap.add_argument("--model", type=Path, default=RW.MODEL)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    det = detector(a.model)
    out = {}
    for lab in a.labels:
        out[lab.name] = r = evaluate(lab, det)
        miss = {k[5:]: v for k, v in r.items() if k.startswith("miss:")}
        false = {k[6:]: v for k, v in r.items() if k.startswith("false:")}
        print(f"{lab.name}: {r['frames']} frames, {r['screws']} screws in the ROI, recall {r['recall']}, "
              f"precision {r['precision']} | missed {miss} | false {false}")
    if a.json:
        a.json.write_text(json.dumps(out, indent=1) + "\n")


if __name__ == "__main__":
    main()
