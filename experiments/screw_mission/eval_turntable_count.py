#!/usr/bin/env python3
"""Screws counted per rotation of the turntable, against the number on the plate.

The frames of a session are fed in order to a detector and to the passage counter of the
device (passage.py). The screws pass the counting line in one burst per rotation; a gap of
more than --gap seconds between two passages separates the rotations. Only rotations that
lie entirely inside the session are scored: a burst within 5 s of either end may be cut.

Detectors:
  blobs   the dark-region detector of the device (200x150, area 3..80), positions as the
          device reports them, the top-left corner of the region
  model   a FOMO int8 model on the region of interest of the frame decoded at 1/4 or 1/8 (--model)

usage: eval_turntable_count.py SESSION... --detector blobs|model [--model M.tflite] [--screws 19]"""

import argparse
import glob
import json
import re
from pathlib import Path

import cv2
import numpy as np

import passage

ROI = (0.30, 0.30, 0.70, 0.70)   # x0, y0, x1, y1 of the model input, fractions of the frame


def frames(session):
    t = {}
    for line in (session / "events.jsonl").read_text().splitlines():
        e = json.loads(line)
        if e.get("event") == "object" and e.get("alias") == 1:
            t.setdefault(e["group"], e["t"])
    out = []
    for f in sorted(glob.glob(str(session / "frame-*.jpg"))):
        seq = int(re.search(r"frame-(\d+)", f).group(1))
        if seq in t:
            out.append((t[seq], f))
    return out


def blobs(path):
    gray = cv2.resize(cv2.imread(path, cv2.IMREAD_GRAYSCALE), (200, 150), interpolation=cv2.INTER_AREA)
    dark = (gray < 0.55 * float(np.median(gray))).astype(np.uint8)
    n, _, stats, _ = cv2.connectedComponentsWithStats(dark, 8)
    out = []
    for k in range(1, n):
        x, y, w, h, area = stats[k]
        if 3 <= area <= 80:
            out.append((x * 256 // 200, y * 256 // 150))
    return out


class Model:
    def __init__(self, path, threshold):
        import tensorflow as tf
        self.it = tf.lite.Interpreter(model_path=str(path))
        self.it.allocate_tensors()
        self.inp, self.out = self.it.get_input_details()[0], self.it.get_output_details()[0]
        _, self.h, self.w, _ = self.inp["shape"]
        self.threshold = threshold

    def __call__(self, path):
        # the decoded frame of which the region is the model input: 400x300 or 200x150
        fw = round(self.w / (ROI[2] - ROI[0]))
        fh = fw * 3 // 4
        gray = cv2.resize(cv2.imread(path, cv2.IMREAD_GRAYSCALE), (fw, fh), interpolation=cv2.INTER_AREA)
        x0, y0, x1, y1 = int(ROI[0] * fw), int(ROI[1] * fh), int(ROI[2] * fw), int(ROI[3] * fh)
        crop = cv2.resize(gray[y0:y1, x0:x1], (self.w, self.h), interpolation=cv2.INTER_AREA)
        scale, zero = self.inp["quantization"]
        q = np.clip(np.round(crop / 255.0 / scale) + zero, -128, 127).astype(np.int8)
        self.it.set_tensor(self.inp["index"], q[None, :, :, None])
        self.it.invoke()
        o = self.it.get_tensor(self.out["index"])[0].astype(np.float32)
        oscale, ozero = self.out["quantization"]
        heat = (o[..., 1] - ozero) * oscale
        return peaks(heat, self.threshold)


def peaks(heat, threshold):
    """The local maxima of the object probability, as the device finds them
    (populate_fomo_peaks in ai_inference_tflm.cc): confidence 0..255, a cell is a maximum when
    no neighbour is higher and no earlier neighbour as high; the position is the cell centre."""
    conf = (np.clip(heat, 0, 1) * 255).astype(np.int32)
    gh, gw = conf.shape
    x0, y0 = 256 * int(ROI[0] * 1000) // 1000, 256 * int(ROI[1] * 1000) // 1000
    xw, yh = 256 * int(ROI[2] * 1000) // 1000 - x0, 256 * int(ROI[3] * 1000) // 1000 - y0
    out = []
    for y in range(gh):
        for x in range(gw):
            c = conf[y, x]
            if c < threshold * 255:
                continue
            peak = True
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if (dx or dy) and 0 <= nx < gw and 0 <= ny < gh:
                        n = conf[ny, nx]
                        if n > c or ((ny, nx) < (y, x) and n == c):
                            peak = False
            if peak:
                out.append((x0 + (2 * x + 1) * xw // (2 * gw), y0 + (2 * y + 1) * yh // (2 * gh)))
    return out


def rotations(times, gap):
    groups, current = [], []
    for t in times:
        if current and t - current[-1] > gap:
            groups.append(current)
            current = []
        current.append(t)
    if current:
        groups.append(current)
    return groups


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("sessions", nargs="+", type=Path)
    p.add_argument("--detector", choices=("blobs", "model"), required=True)
    p.add_argument("--model", type=Path)
    p.add_argument("--threshold", type=float, default=0.5)
    p.add_argument("--screws", type=int, default=19)
    p.add_argument("--gap", type=float, default=15.0)
    p.add_argument("--settled", action="store_true", help="the start correction of the counter")
    p.add_argument("--every", type=int, default=1, help="use every n-th frame")
    a = p.parse_args()
    detect = blobs if a.detector == "blobs" else Model(a.model, a.threshold)
    scored = []
    for session in a.sessions:
        fr = frames(session)[::a.every]
        counter = passage.Counter(settled=a.settled)
        times = []
        for t, f in fr:
            for _ in counter.update(detect(f)):
                times.append(t)
        groups = rotations(times, a.gap)
        start_extra = sum(1 for t in times if t - fr[0][0] < 3.0)
        # a burst that begins or ends within 5 s of an end of the session may be cut
        full = [g for g in groups if g[0] - fr[0][0] > 5.0 and fr[-1][0] - g[-1] > 5.0]
        print(f"{session.name}: {len(fr)} frames over {fr[-1][0] - fr[0][0]:.0f} s, {len(times)} passages, "
              f"bursts {[len(g) for g in groups]}, in the first 3 s {start_extra}")
        scored += [len(g) for g in full]
    if scored:
        err = [c - a.screws for c in scored]
        print(f"complete rotations {len(scored)}: counts {scored}, against {a.screws}: "
              f"mean error {np.mean(err):+.2f}, mean absolute error {np.mean(np.abs(err)):.2f}")


if __name__ == "__main__":
    main()
