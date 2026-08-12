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
    r'<center x="(?P<cx>\d+)"\s+y="(?P<cy>\d+)"\s*/>',
    re.DOTALL)


def parse_xml(path: Path):
    txt = path.read_text()
    out = []
    for m in SPACE_RE.finditer(txt):
        if m.group("occ") == "1":
            out.append((int(m.group("cx")), int(m.group("cy"))))
    return out


def positions(length: int, tile: int, stride: int):
    if length <= tile:
        return [0]
    pos = list(range(0, length - tile + 1, stride))
    last = length - tile
    if pos[-1] != last:
        pos.append(last)
    return pos


def main() -> int:
    ap = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("--root", type=Path,
                    default=root / "datasets/PKLot/PKLot/PKLot")
    ap.add_argument("--out", type=Path,
                    default=root / "datasets/pklot_fomo/vehicle_tiles_labels.jsonl")
    ap.add_argument("--tiles", type=Path,
                    default=root / "datasets/pklot_fomo/tiles160")
    ap.add_argument("--tile-size", type=int, default=160)
    ap.add_argument("--stride", type=int, default=120)
    ap.add_argument("--max-pos", type=int, default=30000)
    ap.add_argument("--max-neg", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=7)
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

            occupied = parse_xml(xml_path)
            if not occupied and args.max_neg == 0:
                continue

            im = Image.open(img_path).convert("L")
            w, h = im.size
            xs = positions(w, args.tile_size, args.stride)
            ys = positions(h, args.tile_size, args.stride)
            tiles = [(x0, y0) for y0 in ys for x0 in xs]
            rng.shuffle(tiles)

            for x0, y0 in tiles:
                objs = []
                for cx, cy in occupied:
                    if x0 <= cx < x0 + args.tile_size and y0 <= cy < y0 + args.tile_size:
                        objs.append({
                            "cls": "car",
                            "conf": 1.0,
                            "cx": round((cx - x0) / args.tile_size, 5),
                            "cy": round((cy - y0) / args.tile_size, 5),
                            "w": 0.0,
                            "h": 0.0,
                        })

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

            if tile_count and tile_count % 1000 == 0:
                print(f"\rtiles {tile_count} pos {pos_count} neg {neg_count} objs {obj_count}", end="", flush=True)

    print(f"\ntiles {tile_count} pos {pos_count} neg {neg_count} objs {obj_count}")
    print(f"wrote {args.out}")
    print(f"tiles dir {args.tiles}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
