#!/usr/bin/env python3
"""
Label archived device frames with a reference detector.

The receiver (host_receiver/picoquic/picohttp/demoserver.c) writes every
reassembled JPEG to AIOT_IMAGE_DIR as frame_<gen_ts_ms>.jpg. The device stamps
the AI object generated from that same capture with the same gen_ts_ms, so
gen_ts_ms is the join key between what the reference detector sees and what the
on-device model scored — no clock alignment between host and device involved.

The reference sees exactly the pixels the device transmitted (160x120 JPEG),
not a better view of the scene. So this measures how much the int8 classifier
gives up against a strong detector *on identical input* — it does not measure
what the sensor's optics threw away.

Usage:
    python3 label_frames.py --frames frames/ --out out/labels.csv
    python3 label_frames.py --frames frames/ --out out/labels.csv --model yolo11s.pt
"""

import argparse
import csv
import json
import os
import re
import sys

FRAME_RE = re.compile(r"frame_(\d+)\.jpg$")


def frame_paths(frames_dir):
    """(gen_ts_ms, path) for every archived frame, oldest first."""
    out = []
    for name in os.listdir(frames_dir):
        m = FRAME_RE.search(name)
        if m:
            out.append((int(m.group(1)), os.path.join(frames_dir, name)))
    out.sort()
    return out


def decodable(path):
    """True when the JPEG decodes fully — a truncated frame raises on load()."""
    from PIL import Image
    try:
        with Image.open(path) as im:
            im.load()
        return True
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", required=True, help="directory of frame_<gen_ts>.jpg")
    ap.add_argument("--out", required=True, help="labels CSV to write")
    ap.add_argument("--model", default="yolo11n.pt",
                    help="ultralytics weights (default yolo11n.pt)")
    ap.add_argument("--imgsz", type=int, default=640,
                    help="inference size; the 160x120 frames are upsampled to it")
    ap.add_argument("--conf", type=float, default=0.10,
                    help="keep detections above this; thresholding for the "
                         "ground-truth decision happens in eval_accuracy.py")
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--device", default="cpu",
                    help="torch device; defaults to cpu because these frames "
                         "are tiny and the ROCm path segfaults here")
    args = ap.parse_args()

    frames = frame_paths(args.frames)
    if not frames:
        sys.exit(f"no frame_<gen_ts>.jpg files in {args.frames}")

    # Image chunks ride unreliable datagrams, so a lost chunk leaves a JPEG
    # that decodes partially or not at all. Drop those here rather than let
    # them decode to garbage and pollute the reference labels; the join in
    # eval_accuracy.py then drops their AI objects too, keeping both sides
    # over exactly the same frames.
    total = len(frames)
    frames = [(ts, p) for ts, p in frames if decodable(p)]
    broken = total - len(frames)
    if broken:
        print(f"skipping {broken}/{total} undecodable frames "
              f"(lost image chunks)", file=sys.stderr)
    if not frames:
        sys.exit("no decodable frames")

    try:
        from ultralytics import YOLO
    except ImportError:
        sys.exit("ultralytics not installed: pip install ultralytics")

    model = YOLO(args.model)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["gen_ts_ms", "n_person", "max_conf", "img_w", "img_h", "boxes"])

        for i in range(0, len(frames), args.batch):
            chunk = frames[i:i + args.batch]
            results = model.predict([p for _, p in chunk], imgsz=args.imgsz,
                                    conf=args.conf, classes=[0], verbose=False,
                                    device=args.device)
            for (gen_ts, _path), res in zip(chunk, results):
                boxes = []
                max_conf = 0.0
                for b in res.boxes:
                    c = float(b.conf[0])
                    max_conf = max(max_conf, c)
                    x1, y1, x2, y2 = (float(v) for v in b.xyxy[0])
                    boxes.append({"conf": round(c, 4),
                                  "xyxy": [round(x1, 1), round(y1, 1),
                                           round(x2, 1), round(y2, 1)]})
                h, w_ = res.orig_shape
                w.writerow([gen_ts, len(boxes), round(max_conf, 4), w_, h,
                            json.dumps(boxes, separators=(",", ":"))])
            print(f"\r labelled {min(i + args.batch, len(frames))}/{len(frames)}",
                  end="", file=sys.stderr, flush=True)

    print(f"\nwrote {args.out} ({len(frames)} frames)", file=sys.stderr)


if __name__ == "__main__":
    main()
