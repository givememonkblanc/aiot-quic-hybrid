#!/usr/bin/env python3
"""Auto-label device frames with a YOLO reference detector for FOMO training.

The receiver stores every reassembled frame in AIOT_IMAGE_DIR
(datasets/vehicle_raw by default). This script runs a full-size YOLO model
over those frames and writes one JSON line per frame with vehicle centroids,
which train_fomo.py consumes. No manual labeling.

Usage:
    python3 autolabel.py [--frames DIR] [--out labels.jsonl]
                         [--model yolo11m.pt] [--conf 0.35] [--min-frames 1]

Vehicle classes (COCO): car(2), motorcycle(3), bus(5), truck(7).
"""
import argparse
import json
import sys
from pathlib import Path

VEHICLE_CLASSES = {2: "car", 3: "motorcycle", 5: "bus", 7: "truck"}


def main() -> int:
    ap = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("--frames", type=Path, default=root / "datasets/vehicle_raw")
    ap.add_argument("--out", type=Path,
                    default=root / "datasets/vehicle_labels.jsonl")
    ap.add_argument("--model", default="yolo11m.pt")
    ap.add_argument("--conf", type=float, default=0.35)
    ap.add_argument("--limit", type=int, default=0, help="cap frames (0 = all)")
    # This host's ROCm build core-dumps in inference; CPU is fast enough for
    # 160x120 frames. Pass --device 0 to try the GPU anyway.
    ap.add_argument("--device", default="cpu")
    args = ap.parse_args()

    from ultralytics import YOLO  # deferred: slow import

    frames = sorted(args.frames.glob("*.jpg"))
    if args.limit:
        frames = frames[: args.limit]
    if not frames:
        print(f"no frames in {args.frames}", file=sys.stderr)
        return 1

    model = YOLO(args.model)
    n_boxes = 0
    with open(args.out, "w") as out:
        for batch_start in range(0, len(frames), 64):
            batch = frames[batch_start : batch_start + 64]
            results = model.predict(
                [str(p) for p in batch], conf=args.conf, verbose=False,
                classes=list(VEHICLE_CLASSES), device=args.device)
            for path, res in zip(batch, results):
                h, w = res.orig_shape
                objs = []
                for box in res.boxes:
                    cx, cy, bw, bh = box.xywhn[0].tolist()
                    objs.append({
                        "cls": VEHICLE_CLASSES[int(box.cls)],
                        "conf": round(float(box.conf), 3),
                        # normalized centroid + size, resolution-independent
                        "cx": round(cx, 4), "cy": round(cy, 4),
                        "w": round(bw, 4), "h": round(bh, 4),
                    })
                n_boxes += len(objs)
                out.write(json.dumps({
                    "frame": path.name, "width": w, "height": h,
                    "objects": objs}) + "\n")
            done = batch_start + len(batch)
            print(f"\r{done}/{len(frames)} frames, {n_boxes} vehicles",
                  end="", flush=True)
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
