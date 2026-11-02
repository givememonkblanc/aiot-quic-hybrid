#!/usr/bin/env python3
"""Conservative static-scene count bound for a frozen FOMO model.

This does not turn output blobs into verified screw identities.  It reports the
best possible recall and minimum misses if every predicted blob were a true,
unique screw, so false positives can only make real performance worse.
"""
import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw
import tensorflow as tf


def percentile(values, q):
    values = sorted(values)
    pos = (len(values) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (pos - lo)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("frames", type=Path)
    p.add_argument("--pattern", default="frame-*.jpg")
    p.add_argument("--model", type=Path,
                   default=Path("experiments/screw_mission/out/screw_fomo_int8.tflite"))
    p.add_argument("--gt-count", type=int, required=True)
    p.add_argument("--threshold", type=float, default=.24)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--review", type=Path)
    a = p.parse_args()
    if a.gt_count <= 0 or not 0 < a.threshold <= 1:
        p.error("gt-count must be positive and threshold in (0,1]")
    frames = sorted(a.frames.glob(a.pattern))
    if not frames:
        p.error("no input frames")
    interpreter = tf.lite.Interpreter(model_path=str(a.model), num_threads=4)
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]
    rows = []
    if a.review:
        a.review.mkdir(parents=True, exist_ok=True)
    for path in frames:
        image = Image.open(path).convert("L")
        x = np.asarray(image.resize((96, 96), Image.Resampling.BILINEAR), np.float32) / 255
        scale, zero = inp["quantization"]
        q = np.clip(np.rint(x / scale + zero), -128, 127).astype(np.int8)[None, ..., None]
        interpreter.set_tensor(inp["index"], q)
        interpreter.invoke()
        scale, zero = out["quantization"]
        heat = (interpreter.get_tensor(out["index"])[0, ..., 1].astype(float) - zero) * scale
        count, _, _, centers = cv2.connectedComponentsWithStats(
            (heat >= a.threshold).astype(np.uint8), 8)
        blobs = count - 1
        upper_matches = min(blobs, a.gt_count)
        row = dict(frame=path.name, output_blobs=blobs,
                   minimum_missed=a.gt_count-upper_matches,
                   recall_upper_bound=upper_matches/a.gt_count,
                   max_score=float(heat.max()))
        rows.append(row)
        if a.review:
            vis = Image.open(path).convert("RGB")
            draw = ImageDraw.Draw(vis)
            for index, (cx, cy) in enumerate(centers[1:], 1):
                px = (cx + .5) * vis.width / heat.shape[1]
                py = (cy + .5) * vis.height / heat.shape[0]
                r = max(8, min(vis.size) // 45)
                draw.ellipse((px-r, py-r, px+r, py+r), outline="red", width=4)
                draw.text((px+r, py-r), str(index), fill="yellow", stroke_width=2,
                          stroke_fill="black")
            vis.save(a.review / path.name)
    counts = [r["output_blobs"] for r in rows]
    best = max(counts)
    report = dict(
        scope="static-scene conservative count bound; blobs are not identity-matched detections",
        model=str(a.model), threshold=a.threshold, gt_count=a.gt_count,
        frames=len(rows), output_blobs_min=min(counts),
        output_blobs_median=percentile(counts, .5), output_blobs_max=best,
        minimum_missed_best_frame=a.gt_count-min(best, a.gt_count),
        recall_upper_bound_best_frame=min(best, a.gt_count)/a.gt_count,
        caveat="False positives and duplicate blobs increase actual misses; use physical event GT for DESR.",
        details=rows)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "details"}, indent=2))


if __name__ == "__main__":
    main()
