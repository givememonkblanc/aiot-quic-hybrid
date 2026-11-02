#!/usr/bin/env python3
"""Screw centroids of turntable frames, for training the screw detector.

The plate is the largest bright region of the frame; its convex hull is the area where
screws are looked for, so that a screw on the rim does not join the dark background. A pixel
in the hull is dark when its luma is below a fraction of the plate luma. Each dark region is
compared with the area of one screw at its height in the frame: nearer screws are larger, so
the area of one screw is a straight line in the row, fitted to the median area of the regions
of each band of rows of the session, in which single screws are the majority. A region of n
screw areas holds n screws, whose centroids are found with k-means on its
pixels. A frame with a region larger than MAX_SCREWS screw areas is marked uncertain and is
left out of training.

usage: label_turntable.py SESSION_DIR... --out labels.jsonl [--sheet review.jpg]"""

import argparse
import glob
import json
from pathlib import Path

import cv2
import numpy as np

WIDTH, HEIGHT = 800, 600
DARK_FRACTION = 0.55
MAX_SCREWS = 5


def plate_hull(gray):
    bright = (gray > 0.8 * np.percentile(gray, 90)).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(bright, 8)
    if n < 2:
        return None
    k = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    pts = cv2.findNonZero((lab == k).astype(np.uint8))
    hull = cv2.convexHull(pts)
    mask = np.zeros_like(bright)
    cv2.fillConvexPoly(mask, hull, 1)
    # a little inside the rim, which is darker than the plate
    return cv2.erode(mask, np.ones((5, 5), np.uint8))


def regions(path, rim=0):
    gray = cv2.resize(cv2.imread(str(path), cv2.IMREAD_GRAYSCALE), (WIDTH, HEIGHT), interpolation=cv2.INTER_AREA)
    hull = plate_hull(gray)
    if hull is None:
        return []
    level = float(np.median(gray[hull > 0]))
    dark = ((gray < DARK_FRACTION * level) & (hull > 0)).astype(np.uint8)
    if rim:
        # A screw lying on the rim is cut by the hull: its dark pixels outside the hull, within rim px of a
        # dark region inside it, are added back. Within rim px only: grown further, a region follows the
        # shadow of the rim along the plate.
        band = cv2.dilate(hull, np.ones((2 * rim + 1, 2 * rim + 1), np.uint8))
        near = cv2.dilate(dark, np.ones((2 * rim + 1, 2 * rim + 1), np.uint8))
        dark = ((gray < DARK_FRACTION * level) & (band > 0) & (near > 0)).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(dark, 8)
    out = []
    for k in range(1, n):
        area = int(stats[k, cv2.CC_STAT_AREA])
        if area < 20:
            continue
        ys, xs = np.nonzero(lab == k)
        out.append(dict(area=area, xs=xs, ys=ys, y=float(ys.mean())))
    return out


def split(region, count):
    pts = np.stack([region["xs"], region["ys"]], 1).astype(np.float32)
    if count <= 1:
        return [pts.mean(0)]
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 0.5)
    _, _, centers = cv2.kmeans(pts, count, None, criteria, 5, cv2.KMEANS_PP_CENTERS)
    return list(centers)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("sessions", nargs="+", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--sheet", type=Path)
    p.add_argument("--rim", type=int, default=0,
                   help="px (at 800x600) outside the plate hull in which a screw on the rim is followed (10/10: 0 = as before)")
    a = p.parse_args()
    rows = []
    for session in a.sessions:
        frames = sorted(glob.glob(str(session / "frame-*.jpg")))
        found = [(f, regions(f, a.rim)) for f in frames]
        all_regions = [r for _, rs in found for r in rs]
        if not all_regions:
            continue
        rows_y = np.array([r["y"] for r in all_regions])
        areas = np.array([r["area"] for r in all_regions])
        bands = [(lo + 30, float(np.median(areas[(rows_y >= lo) & (rows_y < lo + 60)])))
                 for lo in range(0, HEIGHT, 60) if ((rows_y >= lo) & (rows_y < lo + 60)).sum() >= 8]
        # Screws cut by the rim at the far edge are smaller: a band off the line is dropped.
        while True:
            slope, offset = np.polyfit([b[0] for b in bands], [b[1] for b in bands], 1)
            worst = max(bands, key=lambda b: abs(b[1] / (slope * b[0] + offset) - 1))
            if len(bands) <= 3 or abs(worst[1] / (slope * worst[0] + offset) - 1) < 0.1:
                break
            bands.remove(worst)
        single_at = lambda y: max(100.0, slope * y + offset)
        single = single_at(HEIGHT / 2)
        for f, rs in found:
            objects, uncertain = [], False
            for r in rs:
                one = single_at(r["y"])
                n = int(round(r["area"] / one))
                if r["area"] < 0.35 * one:
                    continue
                if n > MAX_SCREWS:
                    uncertain = True
                    continue
                for cx, cy in split(r, max(1, n)):
                    objects.append(dict(cx=float(cx) / WIDTH, cy=float(cy) / HEIGHT, group=n))
            rows.append(dict(frame=str(Path(f).resolve()), session=session.name, single_area=single, uncertain=uncertain, objects=objects))
        print(f"{session.name}: {len(frames)} frames, screw area {single:.0f} px mid-frame ({slope:+.2f} per row) at {WIDTH}x{HEIGHT}, "
              f"{sum(len(r['objects']) for r in rows if r['session'] == session.name)} screws, "
              f"{sum(r['uncertain'] for r in rows if r['session'] == session.name)} uncertain frames")
    a.out.write_text("".join(json.dumps(r) + "\n" for r in rows))
    if a.sheet:
        pick = [r for r in rows if any(o["group"] > 1 for o in r["objects"])][::4][:16] or rows[:16]
        tiles = []
        for r in pick:
            im = cv2.resize(cv2.imread(r["frame"]), (400, 300))
            for o in r["objects"]:
                colour = (0, 255, 0) if o["group"] == 1 else (0, 0, 255)
                cv2.circle(im, (int(o["cx"] * 400), int(o["cy"] * 300)), 4, colour, -1)
            cv2.putText(im, f"{Path(r['frame']).name} n={len(r['objects'])}", (5, 15), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 0, 0), 1)
            tiles.append(im)
        while len(tiles) % 4:
            tiles.append(np.full_like(tiles[0], 255))
        cv2.imwrite(str(a.sheet), np.vstack([np.hstack(tiles[i:i + 4]) for i in range(0, len(tiles), 4)]))


if __name__ == "__main__":
    main()
