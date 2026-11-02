# Mission-Based AIoT Media over QUIC

## Mission

An ESP32-S3 camera observes computer screws on a turntable. For every physical
screw pass, the edge model must deliver a correct detection event and its
associated evidence image before the mission deadline while maintaining the
required preview service.

The primary result is not frame mAP alone:

`Mission success = on-time correct events with valid evidence / all GT passes`

Report missed passes explicitly. Duplicate detections of one pass count once;
an undetected or late pass remains in the denominator. Secondary metrics are
event precision/recall/F1, evidence completion, event age, preview update gap,
capture gap, inference time, goodput, queue bytes, heap and energy when measured.

## Data protocol

Use a white fixed turntable and keep camera position, lighting and focus state
in each session manifest. Record separate physical sessions for train, val and
test. Never split adjacent frames from one rotation across these partitions.

1. Capture empty-table negative sessions.
2. Capture screw-positive sessions across orientation, radius and lighting.
3. Build background-difference centroid labels and inspect every generated
   review image. Correct or reject bad pseudo-labels before training.
4. Train and choose the threshold using train/val only.
5. Freeze the model and threshold.
6. Run a new test session with a manually fixed number of physical passes.
7. Compare FIFO, latest+AI-first and proposed MoQ injection control with the
   same input trace and mission denominator.

Public screw data can initialize the visual encoder, but it cannot replace the
ESP32 installation-domain test set. The live camera currently has optical blur;
the independent test must therefore use the same documented optical state as
deployment.

The current public-data baseline uses the 2026 SortScrews dataset. It contains
560 512x512 images across six screw types plus background, with a separate
28-image validation set. See `RESULTS.md` for the measured float and full-int8
miss counts. Its Apache-2.0 repository code and dataset citation are recorded at
https://github.com/ATATC/SortScrews and https://arxiv.org/abs/2603.13027.

Example collection commands:

```bash
python3 experiments/screw_mission/collect.py \
  --session empty-train-01 --state empty --split train --seconds 60
python3 experiments/screw_mission/collect.py \
  --session screw-train-01 --state screw --split train --seconds 180
python3 experiments/screw_mission/label_background.py
```

Frame-level held-out misses are a model-development result and are kept
separate from physical-pass mission misses:

```bash
python3 experiments/screw_mission/evaluate_misses.py predictions.jsonl \
  --split test --out experiments/screw_mission/results/frame-misses.json
```
