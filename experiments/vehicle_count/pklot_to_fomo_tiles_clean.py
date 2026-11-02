#!/usr/bin/env python3
import argparse
import json
import random
import re
import sys
from pathlib import Path

from PIL import Image

SPACE_RE = re.compile(
    r'<space[^>]*occupied="(?P<occ>[01])">.*?'
    r'<center x="(?P<cx>\d+)"\s+y="(?P<cy>\d+)"\s*/>.*?'
    r'<size w="(?P<w>\d+)"\s+h="(?P<h>\d+)"\s*/>',
    re.DOTALL)


def parse_xml(path: Path):
    txt = path.read_text()
    out = []
    for m in SPACE_RE.finditer(txt):
        out.append({
            "occupied": m.group("occ") == "1",
            "cx": int(m.group("cx")),
            "cy": int(m.group("cy")),
            "w": int(m.group("w")),
            "h": int(m.group("h")),
        })
    return out


def positions(length: int, tile: int, stride: int):
    if length <= tile:
        return [0]
    pos = list(range(0, length - tile + 1, stride))
    last = length - tile
    if pos[-1] != last:
        pos.append(last)
    return pos


def rect_overlap(a, b):
    ax0, ay0, ax1, ay1 = a
    bx0, by0, bx1, by1 = b
    ix0 = max(ax0, bx0)
    iy0 = max(ay0, by0)
    ix1 = min(ax1, bx1)
    iy1 = min(ay1, by1)
    if ix1 <= ix0 or iy1 <= iy0:
        return 0
    return (ix1 - ix0) * (iy1 - iy0)


def main() -> int:
    ap = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("--root", type=Path,
                    default=root / "datasets/PKLot/PKLot/PKLot")
    ap.add_argument("--out", type=Path,
                    default=root / "datasets/pklot_fomo/vehicle_tiles160_clean_labels.jsonl")
    ap.add_argument("--tiles", type=Path,
                    default=root / "datasets/pklot_fomo/tiles160_clean")
    ap.add_argument("--tile-size", type=int, default=160)
    ap.add_argument("--stride", type=int, default=120)
    ap.add_argument("--max-pos", type=int, default=15000)
    ap.add_argument("--max-neg", type=int, default=5000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--neg-overlap-frac", type=float, default=0.02)
    ap.add_argument("--pos-center-margin", type=float, default=0.10)
    ap.add_argument("--pos-min-box-frac", type=float, default=0.35)
    args = ap.parse_args()

    xmls = sorted(args.root.glob("**/*.xml"))
    if not xmls:
        print(f"no XML under {args.root}", file=sys.stderr)
        return 1

    rng = random.Random(args.seed)
    rng.shuffle(xmls)

    args.tiles.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    pos_count = 0
    neg_count = 0
    obj_count = 0
    tile_count = 0

    with open(args.out, "w") as out:
        for xml_path in xmls:
            if args.max_pos > 0 and pos_count >= args.max_pos and args.max_neg > 0 and neg_count >= args.max_neg:
                break

            img_path = xml_path.with_suffix(".jpg")
            if not img_path.exists():
                continue

            spaces = parse_xml(xml_path)
            occupied = [s for s in spaces if s["occupied"]]

            im = Image.open(img_path).convert("L")
            w, h = im.size
            xs = positions(w, args.tile_size, args.stride)
            ys = positions(h, args.tile_size, args.stride)
            tiles = [(x0, y0) for y0 in ys for x0 in xs]
            rng.shuffle(tiles)

            for x0, y0 in tiles:
                tile_rect = (x0, y0, x0 + args.tile_size, y0 + args.tile_size)
                objs = []
                ambiguous = False

                for s in occupied:
                    bw = s["w"]
                    bh = s["h"]
                    box = (
                        s["cx"] - bw / 2.0,
                        s["cy"] - bh / 2.0,
                        s["cx"] + bw / 2.0,
                        s["cy"] + bh / 2.0,
                    )
                    overlap = rect_overlap(tile_rect, box)
                    overlap_frac = overlap / max(1.0, bw * bh)
                    inside_center = x0 <= s["cx"] < x0 + args.tile_size and y0 <= s["cy"] < y0 + args.tile_size

                    if inside_center:
                        mx = args.pos_center_margin * args.tile_size
                        my = args.pos_center_margin * args.tile_size
                        if not (x0 + mx <= s["cx"] < x0 + args.tile_size - mx and
                                y0 + my <= s["cy"] < y0 + args.tile_size - my):
                            ambiguous = True
                            continue
                        if overlap_frac < args.pos_min_box_frac:
                            ambiguous = True
                            continue
                        objs.append({
                            "cls": "car",
                            "conf": 1.0,
                            "cx": round((s["cx"] - x0) / args.tile_size, 5),
                            "cy": round((s["cy"] - y0) / args.tile_size, 5),
                            "w": 0.0,
                            "h": 0.0,
                        })
                    elif overlap_frac >= args.neg_overlap_frac:
                        ambiguous = True

                if ambiguous and not objs:
                    continue

                is_pos = len(objs) > 0
                if is_pos:
                    if args.max_pos > 0 and pos_count >= args.max_pos:
                        continue
                else:
                    if args.max_neg > 0 and neg_count >= args.max_neg:
                        continue

                tile_name = f"{xml_path.stem}_x{x0}_y{y0}.jpg"
                rel = Path(tile_name)
                crop = im.crop((x0, y0, x0 + args.tile_size, y0 + args.tile_size))
                crop.save(args.tiles / rel, quality=95)
                out.write(json.dumps({
                    "frame": str(rel),
                    "width": args.tile_size,
                    "height": args.tile_size,
                    "objects": objs,
                }) + "\n")

                tile_count += 1
                obj_count += len(objs)
                if is_pos:
                    pos_count += 1
                else:
                    neg_count += 1

    print(f"tiles {tile_count} pos {pos_count} neg {neg_count} objs {obj_count}")
    print(f"wrote {args.out}")
    print(f"tiles dir {args.tiles}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
