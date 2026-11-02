#!/usr/bin/env python3
"""Convert SortScrews classification images into reviewable FOMO centroids."""
import argparse
import csv
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw


def load_csv(path):
    return list(csv.DictReader(path.open(newline="")))


def centroid(image_path, background_path, threshold):
    image = cv2.imread(str(image_path), cv2.IMREAD_GRAYSCALE)
    background = cv2.imread(str(background_path), cv2.IMREAD_GRAYSCALE)
    if image is None or background is None or image.shape != background.shape:
        raise ValueError("invalid matched image pair")
    diff = cv2.absdiff(image, background)
    mask = (diff >= threshold).astype(np.uint8)
    # Exclude the colored acquisition border and isolated compression noise.
    mask[:8] = mask[-8:] = 0
    mask[:, :8] = mask[:, -8:] = 0
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    keep = np.zeros_like(mask)
    for i in range(1, count):
        if stats[i, cv2.CC_STAT_AREA] >= 12:
            keep[labels == i] = 1
    weights = diff.astype(np.float64) * keep
    total = weights.sum()
    if total <= 0:
        raise ValueError("no foreground")
    ys, xs = np.indices(image.shape)
    cx = float((xs * weights).sum() / total)
    cy = float((ys * weights).sum() / total)
    yy, xx = np.nonzero(keep)
    return dict(cx=cx/image.shape[1], cy=cy/image.shape[0],
                bbox=[float(xx.min()/image.shape[1]), float(yy.min()/image.shape[0]),
                      float((xx.max()+1)/image.shape[1]), float((yy.max()+1)/image.shape[0])],
                changed_pixels=int(keep.sum()), contrast=float(total))


def convert_set(root, validation, threshold):
    base = root / ("validation" if validation else "")
    images = base / "images"
    records = load_csv(base / "labels.csv")
    per_class = 4 if validation else 20
    setting_span = per_class * 7
    out = []
    for index, row in enumerate(records):
        cls = int(row["class"])
        setting = index // setting_span
        within_class = index % per_class
        bg_index = setting * setting_span + within_class
        split = "public_test" if validation else ("train" if setting < 3 else "val")
        objects = []
        if cls:
            objects = [centroid(images / row["filename"],
                                images / records[bg_index]["filename"], threshold)]
        out.append(dict(frame=str((images / row["filename"]).resolve()),
                        objects=objects, split=split,
                        session=f"sortscrews-{'test' if validation else 'setting'}-{setting}",
                        source="SortScrews", screw_type=cls))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", type=Path,
                   default=Path("datasets/sortscrews/SortScrews"))
    p.add_argument("--out", type=Path,
                   default=Path("datasets/screw_training/public-labels.jsonl"))
    p.add_argument("--threshold", type=int, default=18)
    a = p.parse_args()
    rows = convert_set(a.root, False, a.threshold) + convert_set(a.root, True, a.threshold)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text("".join(json.dumps(r) + "\n" for r in rows))
    review = a.out.parent / "public-review"
    review.mkdir(exist_ok=True)
    positives = [r for r in rows if r["objects"]]
    for i, row in enumerate(positives[::max(1, len(positives)//40)]):
        image = Image.open(row["frame"]).convert("RGB")
        draw = ImageDraw.Draw(image)
        o = row["objects"][0]
        x1, y1, x2, y2 = o["bbox"]
        draw.rectangle((x1*image.width, y1*image.height,
                        x2*image.width, y2*image.height), outline="red", width=4)
        x, y = o["cx"]*image.width, o["cy"]*image.height
        draw.ellipse((x-7, y-7, x+7, y+7), outline="yellow", width=4)
        image.thumbnail((512, 512))
        image.save(review / f"sample-{i:03d}.jpg")
    report = dict(rows=len(rows), positives=len(positives),
                  split_counts={s: sum(r["split"] == s for r in rows)
                                for s in ("train", "val", "public_test")},
                  threshold=a.threshold,
                  note="Public pretraining/development only; not ESP32 mission test")
    (a.out.parent / "public-label-report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
