# TinyML vehicle counting

Scene-specific FOMO model for on-device vehicle counting on the ESP32-S3-EYE
node. The generic person model underperforms; this pipeline trains a tiny
(≈36KB int8) centroid detector **on frames from the actual install site**,
auto-labeled by a full-size YOLO — no manual labeling.

## Workflow

1. **Install & collect.** Point the node at the road (WiFi coverage required).
   Periodic frame streaming is already on (`snapshot.periodic_ms=1000`, plus
   the live stream ≈2.7fps). The receiver must run with
   `AIOT_IMAGE_DIR=datasets/vehicle_raw` (see host_receiver/run_server.sh
   invocation). 30–60 min of traffic ≈ 5–10k frames is a good first dataset.

2. **Auto-label.**
   ```
   python3 autolabel.py            # YOLO11m on CPU (ROCm build core-dumps)
   ```
   Writes `datasets/vehicle_labels.jsonl` (normalized vehicle centroids).

3. **Train + export.**
   ```
   python3 train_fomo.py --epochs 40
   ```
   Watch `cell P/R` (precision/recall at grid-cell level). Outputs in `out/`:
   `fomo_vehicle_int8.tflite`, `ai_inference_vehicle_model.c` (firmware C
   array), `asset_snippet.h` (model asset values).

4. **Deploy.** Drop the C array into `src/components/ai_inference/`, point
   `ai_inference_model_asset_generated.h` at it with
   `.output_type = AI_INFERENCE_OUTPUT_FOMO, .class_count = 1,
   .background_index = 0` (see asset_snippet.h) — the firmware's
   `populate_fomo_detections()` already turns the 12x12 grid into centroid
   detections. Then build + OTA.

## Model contract (must match firmware)

- input `[1,96,96,1]` int8, full-frame squash-resized luma
- output `[1,12,12,2]` int8, channel 0 = background, 1 = vehicle
- per-frame count = activated cells (wire format caps detections at 3;
  count > 3 needs the reserved header byte — TODO with line-crossing)

## Notes

- QQVGA (160x120) source: cars should span ≥ ~10px, i.e. install within
  ~15–20 m of the lane. Raising camera to QVGA needs the 8KB snapshot
  reassembly buffer revisited first.
- Chronological train/val split (last ~15% = val) to avoid leakage from
  near-duplicate consecutive frames.
