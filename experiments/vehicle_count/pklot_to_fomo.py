#!/usr/bin/env python3
"""Convert the PKLot dataset into FOMO training labels for train_fomo.py.

PKLot gives 12,416 full parking-lot images (1280x720) plus one XML per image
that lists every parking space as a polygon with an occupied/vacant flag. We
turn each *occupied* space into one vehicle centroid (its polygon centroid),
normalized to [0,1], and write one jsonl line per image in the exact shape
train_fomo.py consumes:

    {"frame": <rel-path>, "width": W, "height": H,
     "objects": [{"cls":"car","cx":0.5,"cy":0.4,...}, ...]}

The frames directory passed to train_fomo.py is the PKLot root, so `frame` is
the image path relative to that root (train_fomo does frames_dir / frame).

Usage:
    python3 pklot_to_fomo.py [--root datasets/PKLot/PKLot/PKLot]
                             [--out datasets/pklot_fomo/vehicle_labels.jsonl]
                             [--max-pos N] [--max-neg N]
                             [--seed 7]

--max-pos/--max-neg cap how many positive (>=1 vehicle) / negative (0 vehicle)
images are emitted, for a bounded, class-balanced training set. 0 = no cap.
"""
import argparse
import json
import random
import re
import sys
from pathlib import Path

SPACE_RE = re.compile(
    r'<space[^>]*occupied="(?P<occ>[01])">.*?'
    r'<center x="(?P<cx>\d+)"\s+y="(?P<cy>\d+)"\s*/>',
    re.DOTALL)


def parse_xml(path: Path):
    """Return list of (occupied, cx, cy) for each parking space."""
    txt = path.read_text()
    out = []
    for m in SPACE_RE.finditer(txt):
        out.append((m.group("occ") == "1",
                    int(m.group("cx")), int(m.group("cy"))))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("--root", type=Path,
                    default=root / "datasets/PKLot/PKLot/PKLot")
    ap.add_argument("--out", type=Path,
                    default=root / "datasets/pklot_fomo/vehicle_labels.jsonl")
    ap.add_argument("--max-pos", type=int, default=0,
                    help="cap positive (with vehicle) images, 0=all")
    ap.add_argument("--max-neg", type=int, default=0,
                    help="cap negative (empty) images, 0=all")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    xmls = sorted(args.root.glob("**/*.xml"))
    if not xmls:
        print(f"no XML under {args.root}", file=sys.stderr)
        return 1

    pos, neg = [], []
    for x in xmls:
        spaces = parse_xml(x)
        vehicles = [s for s in spaces if s[0]]
        (pos if vehicles else neg).append((x, vehicles))

    rng = random.Random(args.seed)
    rng.shuffle(pos)
    rng.shuffle(neg)
    if args.max_pos > 0:
        pos = pos[: args.max_pos]
    if args.max_neg > 0:
        neg = neg[: args.max_neg]
    entries = pos + neg
    rng.shuffle(entries)  # interleave classes for the chronological-free split

    args.out.parent.mkdir(parents=True, exist_ok=True)
    n_obj = 0
    with open(args.out, "w") as out:
        for x, vehicles in entries:
            img = x.with_suffix(".jpg")
            rel = img.relative_to(args.root)
            from PIL import Image
            im = Image.open(img)
            w, h = im.size
            objs = [{
                "cls": "car",
                "conf": 1.0,
                "cx": round(cx / w, 5),
                "cy": round(cy / h, 5),
                "w": 0.0,
                "h": 0.0,
            } for _, cx, cy in vehicles]
            n_obj += len(objs)
            out.write(json.dumps({
                "frame": str(rel), "width": w, "height": h,
                "objects": objs}) + "\n")

    print(f"positive (with vehicle): {len(pos)}  "
          f"negative (empty): {len(neg)}")
    print(f"total images: {len(entries)}  vehicle centroids: {n_obj}")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
