#!/usr/bin/env python3
"""Create reviewable screw-centroid labels using fixed-scene background differencing."""
import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def gray(path, size):
    return cv2.resize(cv2.imread(str(path), cv2.IMREAD_GRAYSCALE), size,
                      interpolation=cv2.INTER_AREA)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", type=Path, default=Path("datasets/screw_live"))
    p.add_argument("--out", type=Path, default=Path("datasets/screw_training/labels.jsonl"))
    p.add_argument("--threshold", type=int, default=24)
    p.add_argument("--min-area", type=int, default=20)
    p.add_argument("--max-area", type=int, default=5000)
    p.add_argument("--work-width", type=int, default=320)
    p.add_argument("--work-height", type=int, default=240)
    a = p.parse_args()
    manifests = sorted(a.root.glob("*/manifest.jsonl"))
    all_rows = [(m.parent, r) for m in manifests for r in rows(m)]
    empty = [(d, r) for d, r in all_rows if r["state"] == "empty"]
    positive = [(d, r) for d, r in all_rows if r["state"] == "screw"]
    if not empty or not positive:
        raise SystemExit("need at least one empty and one screw session")
    size = (a.work_width, a.work_height)
    bg_samples = [gray(d / r["frame"], size) for d, r in empty]
    background = np.median(np.stack(bg_samples), axis=0).astype(np.uint8)
    kernel = np.ones((3, 3), np.uint8)
    output = []
    rejected = []
    review = a.out.parent / "review"
    review.mkdir(parents=True, exist_ok=True)
    for d, r in empty:
        output.append(dict(frame=str((d / r["frame"]).resolve()), objects=[],
                           session=r["session"], split=r["split"], state="empty"))
    for index, (d, r) in enumerate(positive):
        image_path = d / r["frame"]
        image = gray(image_path, size)
        diff = cv2.absdiff(image, background)
        mask = (diff >= a.threshold).astype(np.uint8) * 255
        mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=2)
        count, _, stats, centers = cv2.connectedComponentsWithStats(mask, 8)
        candidates = []
        for component in range(1, count):
            x, y, w, h, area = map(int, stats[component])
            if a.min_area <= area <= a.max_area:
                # Prefer a compact, high-contrast foreground component.
                score = float(diff[y:y+h, x:x+w].sum())
                candidates.append((score, x, y, w, h, area, centers[component]))
        if not candidates:
            rejected.append(dict(frame=str(image_path), reason="no_component"))
            continue
        score, x, y, w, h, area, center = max(candidates)
        cx, cy = map(float, center)
        obj = dict(cx=cx/a.work_width, cy=cy/a.work_height,
                   bbox=[x/a.work_width, y/a.work_height,
                         (x+w)/a.work_width, (y+h)/a.work_height],
                   area=area, contrast_score=score)
        output.append(dict(frame=str(image_path.resolve()), objects=[obj],
                           session=r["session"], split=r["split"], state="screw"))
        if index % max(1, len(positive)//40) == 0:
            vis = Image.open(image_path).convert("RGB")
            draw = ImageDraw.Draw(vis)
            sx, sy = vis.width/a.work_width, vis.height/a.work_height
            draw.rectangle((x*sx, y*sy, (x+w)*sx, (y+h)*sy), outline="red", width=5)
            vis.thumbnail((640, 480))
            vis.save(review / f"{r['session']}-{index:05d}.jpg")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text("".join(json.dumps(r) + "\n" for r in output))
    report = dict(empty=len(empty), screw=len(positive), labeled=len(output)-len(empty),
                  rejected=len(rejected), threshold=a.threshold,
                  sessions=sorted({r["session"] for _, r in all_rows}),
                  split_counts={s: sum(r["split"] == s for r in output)
                                for s in ("train", "val", "test")})
    (a.out.parent / "label-report.json").write_text(json.dumps({**report, "rejects": rejected}, indent=2))
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
