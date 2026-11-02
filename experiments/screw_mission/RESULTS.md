# Mission-Based AIoT Media over QUIC: screw baseline

Status: static 19-object ESP32 characterization complete. No physical
turntable passage mission result yet.

## Data

- SortScrews: 560 development images plus 28 official validation images, six
  screw types and background. The acquisition-setting split is frozen at
  420 train / 140 validation / 28 public test images.
- Public test denominator: 24 screws and four empty images.
- ESP32 installation data: 35 unique 1600x1200 empty-turntable frames collected
  as `empty-train-20260922-01`. No screw was visible in the live camera during
  collection, so these are negative examples only.
- The public data is a pretraining and pipeline baseline. It is not used to
  claim performance on the blurred ESP32 installation.

## Model pilot

The prototype is a one-class 96x96 luma FOMO model with a 12x12 output grid.
Train and validation are separated by physical capture setting. The public test
partition is not used to choose the threshold.

| Model stage | Threshold source | Detected / GT | Missed | FP | Precision | Recall | F1 |
|---|---|---:|---:|---:|---:|---:|---:|
| PyTorch float | float validation | 23 / 24 | 1 | 6 | 0.793 | 0.958 | 0.868 |
| TFLite full-int8 | int8 validation | 13 / 24 | 11 | 15 | 0.464 | 0.542 | 0.500 |

The first training attempt is retained in `results/public-v1-failed`: it made
only 50 optimizer updates and missed 22/24 public-test screws. Mini-batch
training corrected that implementation error. The remaining float-to-int8
drop is a measured deployment problem, not a transport result. The int8 model
was subsequently flashed only as a bounded characterization build; it has not
been accepted as the installation model.

## Static 19-screw ESP32 characterization (2026-09-23)

The first live model run was invalid for counting because the AI result format
silently capped each result at three detections. After increasing that capacity
to 24, the ESP32 produced 22 AI results containing 3--5 blobs each (median 4,
maximum 5). With 19 physical screws in the scene, the best observed frame thus
missed **at least 14/19**, even under the optimistic assumption that all five
reported blobs were true screws. The new result capacity was never hit.

The corrected run used MoQT-21 over a 128 kbit/s uplink with 50 ms added delay.
It delivered all 22 AI objects, 10 UXGA previews, and recorded zero shaper
packet drops. Source-to-ACK was 5.113 s p50 and 5.257 s p95. This separates the
poor static count from the earlier three-entry result truncation and from link
loss. Full evidence and limitations are in `ESP32_19_RESULTS.md`.

## Mission contract

One physical screw passage is one GT event. It is successful only when a
correct detection and valid associated evidence image arrive before that
event's fixed deadline. Missed, late and invalid-evidence events remain in the
GT denominator. Preview freshness and resource limits are simultaneous
constraints, so dropping every preview is not a valid optimum.

The next valid result requires positive train and validation sessions from the
installed ESP32 camera, a newly frozen int8 model, and a separate test session
with a manually fixed number of turntable passages. `evaluate_events.py`
computes the final mission success rate from those GT events and received
detections.

## Live positive-collection attempt (2026-09-22)

The first 45-second `screw-train-20260922-01` attempt saved zero frames because
`latest.jpg` stopped updating at 23:08:48 KST. The ESP32 continued publishing
RAM telemetry, but its camera counter remained at 55,112. Restarting the QUIC
receiver did not recover the client session. A bounded OTA reboot was then
performed through archived `camera-uxga-v1`, followed by restoration of the
deployed `camera-uxga-q6-v2` image. Network delivery recovered (about 68--71 ms
RTT immediately after reboot), while both images reported `frames=0` and
`ai_us=0`. This isolates the immediate failure to camera initialization or
capture rather than the receiver, QUIC path, labeling code, or screw model.
No unverified positive sample was added to the dataset.

After USB power was restored on 2026-09-23, capture recovered: telemetry showed
3,107 frames in 259.3 seconds (about 12.0 capture FPS), 5.48-second legacy-FOMO
inference, 20 ms QUIC RTT, and a 154,438-byte peak transport queue. A 60-second
candidate positive session saved 78 unique JPEGs. It is quarantined rather than
used for training: the camera had moved, the turntable was almost entirely
outside the field of view, and all eight background-difference proposals were
scene-edge artifacts. A new same-pose empty/positive pair is required.

After transition back to remote power/location, a 31.0-second health window
confirmed continued operation: 367 new camera frames (11.84 FPS), changing
JPEG output, QUIC RTT 25--32 ms, and 10,470--35,593 queued bytes. The scene is
not mission-valid: a dark near-field surface occupies most of the frame and
neither the turntable nor a screw is identifiable. Raw counters are retained
in `results/remote-health-20260923.json`; no frames from this pose are labeled.
