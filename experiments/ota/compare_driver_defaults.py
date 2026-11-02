"""Compare focus metrics for the controlled OV2640 driver-defaults A/B test."""

import json
from pathlib import Path
import statistics

import cv2
import numpy as np


ROOT = Path(__file__).resolve().parents[2]
RESULTS = ROOT / "experiments/ota/results/driver-defaults"
GROUPS = ("current-before", "defaults", "current-after")


def metrics(path: Path) -> dict[str, float]:
    image = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if image is None or image.shape != (1200, 1600):
        raise RuntimeError(f"unexpected image: {path}")
    # The card occupies the lower portion in every fixed-camera sample.
    roi = image[560:1200, 250:1550]
    normalized = cv2.normalize(roi, None, 0, 255, cv2.NORM_MINMAX)
    laplacian = cv2.Laplacian(normalized, cv2.CV_64F)
    gx = cv2.Sobel(normalized, cv2.CV_64F, 1, 0, ksize=3)
    gy = cv2.Sobel(normalized, cv2.CV_64F, 0, 1, ksize=3)
    return {
        "luma_mean": float(roi.mean()),
        "luma_std": float(roi.std()),
        "laplacian_variance_normalized": float(laplacian.var()),
        "tenengrad_normalized": float(np.mean(gx * gx + gy * gy)),
    }


report = {"roi_xywh": [250, 560, 1300, 640], "groups": {}}
for group in GROUPS:
    rows = [metrics(path) for path in sorted((RESULTS / group).glob("frame-*.jpg"))]
    if not rows:
        raise RuntimeError(f"no samples for {group}")
    report["groups"][group] = {
        "frames": len(rows),
        "median": {key: statistics.median(row[key] for row in rows) for key in rows[0]},
        "samples": rows,
    }

(RESULTS / "focus-metrics.json").write_text(json.dumps(report, indent=2) + "\n")
for group in GROUPS:
    print(group, report["groups"][group]["median"])
