#!/usr/bin/env python3
import argparse
import csv
import json
import random
import sys
from pathlib import Path

from PIL import Image

SRC_W = 2592.0
SRC_H = 1944.0
DST_W = 1000.0
DST_H = 750.0


def load_camera_boxes(base: Path):
    cams = {}
    for csv_path in sorted(base.glob('camera*.csv')):
        cam_id = csv_path.stem.replace('camera', '').zfill(2)
        slots = {}
        with csv_path.open() as f:
            r = csv.DictReader(f)
            for row in r:
                slot = row['SlotId']
                x = float(row['X']) * DST_W / SRC_W
                y = float(row['Y']) * DST_H / SRC_H
                w = float(row['W']) * DST_W / SRC_W
                h = float(row['H']) * DST_H / SRC_H
                slots[slot] = (x, y, w, h)
        cams[cam_id] = slots
    return cams


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
        return 0.0
    return float((ix1 - ix0) * (iy1 - iy0))


def main() -> int:
    ap = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[2]
    ap.add_argument('--images', type=Path,
                    default=root / 'datasets/cnrpark/CNR-EXT_FULL_IMAGE_1000x750/FULL_IMAGE_1000x750')
    ap.add_argument('--meta', type=Path,
                    default=root / 'datasets/cnrpark/CNRPark+EXT.csv')
    ap.add_argument('--boxes', type=Path,
                    default=root / 'datasets/cnrpark/CNR-EXT_FULL_IMAGE_1000x750')
    ap.add_argument('--out', type=Path,
                    default=root / 'datasets/cnrpark/cnrext_tiles160_labels.jsonl')
    ap.add_argument('--tiles', type=Path,
                    default=root / 'datasets/cnrpark/cnrext_tiles160')
    ap.add_argument('--tile-size', type=int, default=160)
    ap.add_argument('--stride', type=int, default=120)
    ap.add_argument('--max-pos', type=int, default=15000)
    ap.add_argument('--max-neg', type=int, default=5000)
    ap.add_argument('--seed', type=int, default=7)
    ap.add_argument('--neg-overlap-frac', type=float, default=0.02)
    ap.add_argument('--pos-center-margin', type=float, default=0.10)
    ap.add_argument('--pos-min-box-frac', type=float, default=0.35)
    args = ap.parse_args()

    cam_boxes = load_camera_boxes(args.boxes)
    rows = []
    with args.meta.open() as f:
        r = csv.DictReader(f)
        for row in r:
            cam = row['camera'].zfill(2)
            if not cam.isdigit() or cam not in cam_boxes:
                continue
            slot = row['slot_id']
            if slot not in cam_boxes[cam]:
                continue
            weather = {'S': 'SUNNY', 'O': 'OVERCAST', 'R': 'RAINY'}[row['weather']]
            dt = row['datetime']
            date, hm = dt.split('_', 1)
            hm = hm.replace('.', '')
            image_rel = Path('FULL_IMAGE_1000x750') / weather / date / f'camera{int(cam)}' / f'{date}_{hm}.jpg'
            rows.append((str(image_rel), int(row['occupancy']), slot, cam_boxes[cam][slot]))

    by_image = {}
    for image_rel, occ, slot, box in rows:
        by_image.setdefault(image_rel, []).append((occ, slot, box))

    items = list(by_image.items())
    rng = random.Random(args.seed)
    rng.shuffle(items)

    args.tiles.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    pos_count = neg_count = obj_count = tile_count = 0

    with open(args.out, 'w') as out:
        for image_rel, slots in items:
            if args.max_pos > 0 and pos_count >= args.max_pos and args.max_neg > 0 and neg_count >= args.max_neg:
                break
            img_path = args.images.parent / image_rel
            if not img_path.exists():
                continue
            occupied = [box for occ, _, box in slots if occ == 1]
            if not occupied and args.max_neg == 0:
                continue

            im = Image.open(img_path).convert('L')
            w, h = im.size
            xs = positions(w, args.tile_size, args.stride)
            ys = positions(h, args.tile_size, args.stride)
            tile_positions = [(x0, y0) for y0 in ys for x0 in xs]
            rng.shuffle(tile_positions)

            for x0, y0 in tile_positions:
                tile_rect = (x0, y0, x0 + args.tile_size, y0 + args.tile_size)
                objs = []
                ambiguous = False
                for cx, cy, bw, bh in occupied:
                    box = (cx - bw / 2.0, cy - bh / 2.0, cx + bw / 2.0, cy + bh / 2.0)
                    overlap = rect_overlap(tile_rect, box)
                    overlap_frac = overlap / max(1.0, bw * bh)
                    inside_center = x0 <= cx < x0 + args.tile_size and y0 <= cy < y0 + args.tile_size
                    if inside_center:
                        mx = args.pos_center_margin * args.tile_size
                        my = args.pos_center_margin * args.tile_size
                        if not (x0 + mx <= cx < x0 + args.tile_size - mx and y0 + my <= cy < y0 + args.tile_size - my):
                            ambiguous = True
                            continue
                        if overlap_frac < args.pos_min_box_frac:
                            ambiguous = True
                            continue
                        objs.append({
                            'cls': 'car',
                            'conf': 1.0,
                            'cx': round((cx - x0) / args.tile_size, 5),
                            'cy': round((cy - y0) / args.tile_size, 5),
                            'w': 0.0,
                            'h': 0.0,
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

                tile_name = f"{Path(image_rel).stem}_x{x0}_y{y0}.jpg"
                crop = im.crop((x0, y0, x0 + args.tile_size, y0 + args.tile_size))
                crop.save(args.tiles / tile_name, quality=95)
                out.write(json.dumps({
                    'frame': tile_name,
                    'width': args.tile_size,
                    'height': args.tile_size,
                    'objects': objs,
                }) + '\n')

                tile_count += 1
                obj_count += len(objs)
                if is_pos:
                    pos_count += 1
                else:
                    neg_count += 1

    print(f'tiles {tile_count} pos {pos_count} neg {neg_count} objs {obj_count}')
    print(f'wrote {args.out}')
    print(f'tiles dir {args.tiles}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
