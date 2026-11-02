#!/usr/bin/env python3
"""Quality of the evidence representations against the source image.

For every camera frame of full resolution found in the recorded runs, the medium and low
representations are produced with the parameters of the ladder (half and quarter size, JPEG
quality 75 and 65) and compared with the source after scaling back. Reported: SSIM and the
share of the local detail (gradient energy) that survives. The scheduler uses SSIM as the
quality of a representation."""

import argparse
import hashlib
import json
import statistics
from pathlib import Path

import cv2
import numpy as np
from skimage.metrics import structural_similarity

LEVELS = dict(medium=(0.5, 75), low=(0.25, 65))


def representation(image, scale, quality):
    h, w = image.shape[:2]
    small = cv2.resize(image, (round(w * scale), round(h * scale)), interpolation=cv2.INTER_AREA)
    ok, data = cv2.imencode(".jpg", small, [cv2.IMWRITE_JPEG_QUALITY, quality])
    assert ok
    decoded = cv2.imdecode(data, cv2.IMREAD_COLOR)
    return len(data), cv2.resize(decoded, (w, h), interpolation=cv2.INTER_CUBIC)


def detail(gray):
    gx, gy = cv2.Sobel(gray, cv2.CV_32F, 1, 0), cv2.Sobel(gray, cv2.CV_32F, 0, 1)
    return float(np.mean(gx * gx + gy * gy))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--min-width", type=int, default=1600)
    a = p.parse_args()
    seen, rows = set(), []
    for path in sorted(a.root.rglob("frame-*.jpg")):
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if digest in seen:
            continue
        image = cv2.imdecode(np.frombuffer(raw, np.uint8), cv2.IMREAD_COLOR)
        if image is None or image.shape[1] < a.min_width:
            continue
        seen.add(digest)
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        row = dict(file=str(path.relative_to(a.root)), sha256=digest, width=image.shape[1], height=image.shape[0],
                   bytes=len(raw))
        for name, (scale, quality) in LEVELS.items():
            size, back = representation(image, scale, quality)
            g = cv2.cvtColor(back, cv2.COLOR_BGR2GRAY)
            row[name] = dict(bytes=size, ssim=float(structural_similarity(gray, g, data_range=255)),
                             detail=detail(g) / max(1e-9, detail(gray)))
        rows.append(row)
    summary = dict(frames=len(rows), source_bytes_median=statistics.median(r["bytes"] for r in rows))
    for name in LEVELS:
        for key in ("bytes", "ssim", "detail"):
            v = sorted(r[name][key] for r in rows)
            summary[f"{name}_{key}"] = dict(median=statistics.median(v), p10=v[round(.1 * (len(v) - 1))],
                                           p90=v[round(.9 * (len(v) - 1))])
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "representation-quality.json").write_text(json.dumps(dict(schema=1, levels=LEVELS, summary=summary, frames=rows),
                                                                  indent=1) + "\n")
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
