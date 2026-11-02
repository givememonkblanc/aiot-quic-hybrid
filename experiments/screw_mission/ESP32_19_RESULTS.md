# ESP32 19-screw static characterization

Date: 2026-09-23 (Asia/Seoul)

This test answers the narrow question: with 19 screws placed in the current
camera scene, how many blobs does the deployed public-data FOMO model report in
one frame? It is not yet a turntable passage-success experiment.

## Correction to the first run

The original AI result structure held only three detections. All 22 results
from `moqt-screw-public-cap32-v13-shaped128-20260923-r1` therefore reported
exactly three and hit the serialization capacity. That run cannot measure the
model's object count.

The result capacity was increased from 3 to 24 and the MoQ JSON buffer was
made proportional to the maximum binary result size. The same public int8
model and transport policy were rebuilt as `moqt-screw-public-cap24-v14`.

## Corrected result

Network condition: 128 kbit/s device-to-host, 50 ms added delay, 64-packet
queue. Camera preview was 1600x1200 JPEG and the preview-unacknowledged budget
was 32 KiB.

| Metric | Result |
|---|---:|
| Physical screws stated by operator | 19 |
| ESP32 AI results received | 22 |
| Reported blobs per result | 3--5 |
| Median reported blobs | 4 |
| Best-frame reported blobs | 5 |
| Frames hitting the new 24-entry capacity | 0 |
| Minimum misses in best frame, if all blobs are true screws | **14 / 19** |
| Misses at median reported count, same optimistic assumption | **15 / 19** |
| Best-frame recall ceiling | **26.3%** |

The answer is therefore at least 14 missed screws in the best observed frame.
This is an optimistic lower bound: a false-positive blob is counted as a
screw, so position-level ground-truth matching can only keep or increase the
miss count.

## Transport result from the same run

| Metric | Result |
|---|---:|
| Preview objects / AI objects | 10 / 22 |
| Preview payload goodput | 105.0 kbit/s |
| AI capture-to-observed p50 / p95 | 3.005 / 3.122 s |
| AI source-to-ACK p50 / p95 | 5.113 / 5.257 s |
| Shaper packet drops | 0 / 947 |
| OTA restoration | verified |

The 164-byte maximum binary AI payload and its JSON representation fit the
existing MoQ AI track. Expanding the detection count did not introduce a
network loss in this trial. The poor count is therefore a model/camera-domain
problem rather than a MoQ result-capacity or shaped-link drop in this run.

## Evidence and reproducibility

- `results/current-19-esp32-cap3.json`: first, truncated run
- `results/current-19-esp32-cap24.json`: corrected wire-level count and boxes
- `results/current-19-esp32-cap24-overlay.jpg`: the one preview frame whose
  source sequence exactly matches an AI result
- `evaluate_moq_count.py`: validates JSON count against the binary wire count
- `../moq/results/moqt-screw-public-cap24-v14-shaped128-20260923-r1`: raw run

The next mission result must assign one ground-truth event to every screw
passage and match detections by time and position. That produces exact missed,
late, and successfully delivered event counts; repeated observations of the
same static scene cannot provide those values.
