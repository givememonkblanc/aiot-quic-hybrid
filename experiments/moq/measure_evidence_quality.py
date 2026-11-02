#!/usr/bin/env python3
"""Task quality of the evidence representations: what a reference detector still finds.

Evidence serves the examination of a reported event. Its quality is therefore measured as
the share of the objects that a reference detector finds in the source image and finds
again, at the same place, in a representation. Images: CNRPark+EXT full frames (parking
lot, vehicles). Reference detector: YOLO11m. Representations as in the ladder: full size,
half size at JPEG quality 75, quarter size at JPEG quality 65."""

import argparse
import json
import random
import statistics
from pathlib import Path

import cv2
import numpy as np
from ultralytics import YOLO

LEVELS = dict(high=(1.0, None), medium=(0.5, 75), low=(0.25, 65))
VEHICLES = {2, 3, 5, 7}  # car, motorcycle, bus, truck


def representation(image, scale, quality):
    if quality is None:
        ok, data = cv2.imencode(".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, 90])
        return len(data), image
    h, w = image.shape[:2]
    small = cv2.resize(image, (round(w * scale), round(h * scale)), interpolation=cv2.INTER_AREA)
    ok, data = cv2.imencode(".jpg", small, [cv2.IMWRITE_JPEG_QUALITY, quality])
    return len(data), cv2.imdecode(data, cv2.IMREAD_COLOR)


def iou(a, b):
    x1, y1, x2, y2 = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def boxes(model, image, scale, confidence):
    r = model.predict(image, imgsz=max(image.shape[:2]) // 32 * 32 or 32, conf=confidence, verbose=False, device="cpu")[0]
    out = []
    for b, c in zip(r.boxes.xyxy.cpu().numpy(), r.boxes.cls.cpu().numpy()):
        if int(c) in VEHICLES:
            out.append([float(v) / scale for v in b])
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--images", type=Path, required=True)
    p.add_argument("--model", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--count", type=int, default=300)
    p.add_argument("--seed", type=int, default=20260929)
    p.add_argument("--confidence", type=float, default=0.35)
    a = p.parse_args()
    files = sorted(a.images.rglob("*.jpg"))
    random.Random(a.seed).shuffle(files)
    model = YOLO(str(a.model))
    rows = []
    for path in files:
        if len(rows) >= a.count:
            break
        image = cv2.imread(str(path))
        if image is None:
            continue
        reference = boxes(model, image, 1.0, a.confidence)
        if len(reference) < 3:
            continue
        row = dict(file=str(path.relative_to(a.images)), objects=len(reference))
        for name, (scale, quality) in LEVELS.items():
            size, rep = representation(image, scale, quality)
            found = boxes(model, rep, scale, a.confidence)
            kept = sum(1 for r in reference if any(iou(r, f) >= 0.5 for f in found))
            row[name] = dict(bytes=size, kept=kept, found=len(found), width=rep.shape[1], height=rep.shape[0])
        rows.append(row)
    total = sum(r["objects"] for r in rows)
    summary = dict(images=len(rows), objects=total, model=a.model.name, confidence=a.confidence)
    for name in LEVELS:
        kept = sum(r[name]["kept"] for r in rows)
        per = [r[name]["kept"] / r["objects"] for r in rows]
        summary[name] = dict(quality=kept / total, per_image_median=statistics.median(per),
                             per_image_p10=sorted(per)[round(.1 * (len(per) - 1))],
                             bytes_median=statistics.median(r[name]["bytes"] for r in rows))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "evidence-quality.json").write_text(json.dumps(dict(schema=1, levels=LEVELS, summary=summary, images=rows),
                                                            indent=1) + "\n")
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
