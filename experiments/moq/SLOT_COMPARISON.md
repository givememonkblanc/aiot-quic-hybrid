# One versus two JPEG objects in flight

The comparison changes only `AIOT_MOQT_IMAGE_SLOTS` (1 or 2) on the bounded-pump,
oldest-JPEG configuration. Control/subscription streams and the reserved AI
result slot are unchanged. A JPEG slot remains occupied until the QUIC stream
closes after acknowledgement; this is not a QUIC packet/window limit.

Both settings use ESPDet cat224 on CPU1, 1024×1024 camera JPEG, a 100 ms capture
release grid, one latest preview cache, and 60 s collection followed by 10 s
drain/AI observation. The original FOMO firmware is restored after each run.
Live camera input and received image sizes are recorded; these are sequential
exploratory runs, not an identical replay or randomized statistical comparison.

## Hypothesis and limits

Two slots may overlap acknowledgement waiting and improve complete throughput,
but the second image also waits behind an earlier image's submission. One slot
can eliminate that extra admitted-image wait. It cannot remove the cost of
packetization, encryption, Wi-Fi, or camera/AI resource contention.

Evaluate complete received images against **all acquired frames**, not only
admitted objects. Latest-cache replacements remain omissions. Slot residency
is a device-local admission-to-close interval, not measured exposure-to-server
latency or formal AoI. Actual AI runs and all result receipts are counted;
zero-detection cat224 results are not GT screw-detection success.

## Reproduce

```
AIOT_MOQT_CONCURRENT=ON AIOT_MOQT_BOUNDED_PUMP=ON AIOT_MOQT_IMAGE_SLOTS=1 bash experiments/moq/build.sh
experiments/moq/vendor/venv/bin/python experiments/moq/device_trial.py --dual --image-slots 1 --name UNIQUE_ONE_SLOT_RUN

AIOT_MOQT_CONCURRENT=ON AIOT_MOQT_BOUNDED_PUMP=ON AIOT_MOQT_IMAGE_SLOTS=2 bash experiments/moq/build.sh
experiments/moq/vendor/venv/bin/python experiments/moq/device_trial.py --dual --image-slots 2 --name UNIQUE_TWO_SLOT_RUN
```

The harness checks the boot configuration against the requested slot count,
checks actual model execution, matches JPEG hashes and AI source sequences, and
verifies both tracks' finite publication lifecycle. A functional pass is not a
100 ms acquisition deadline or mission-QoS pass. The shared slot-order regression
also runs with ASan/UBSan. Only the transport component receives the slot-count
compile definition; the model and acquisition code remain unchanged.

## Results: fresh sequential pair

| Measurement | 1 JPEG slot | 2 JPEG slots |
|---|---:|---:|
| Captured / complete received JPEG | 601 / 34 | 601 / 41 |
| Complete images / all captures | 5.66% | 6.82% |
| Mean acquisition rate | 9.999 Hz | 9.999 Hz |
| Maximum acquisition gap | 219.0 ms | 179.8 ms |
| JPEG slot residency p50 | 1.731 s | 2.925 s |
| JPEG slot residency p95 / max | 1.825 / 1.887 s | 3.092 / 3.267 s |
| Mean received JPEG size | 141,257 B | 131,573 B |
| Complete JPEG payload from the 60 s source interval | 4.803 MB | 5.394 MB |
| Actual model executions / AI results received | 24 / 24 | 25 / 25 |
| AI inference p50 / p95 | 2.510 / 2.574 s | 2.467 / 2.548 s |

The one-slot run reduced typical slot residence by **40.8%** in this pair, with
17.1% fewer complete images and 11.0% less complete JPEG payload. Its images were
also about 7.4% larger on average. Therefore those throughput differences cannot
be assigned solely to slot count. The p50 model-execution difference is only
about 1.8%; reducing the JPEG queue did not materially resolve AI runtime here.
These are one run per setting, not confidence intervals or a universal ranking.

Collection accounting:

- 1 slot: 601 = 11 cache-lock misses + 555 cache replacements + 34 admissions +
  1 cached frame remaining at cutoff.
- 2 slots: 601 = 8 cache-lock misses + 551 cache replacements + 41 admissions +
  1 cached frame remaining at cutoff.

All admitted images were hash-matched and decoded; observed AI results were
received with their source identities intact. Both trials completed both tracks
and normal session closure. No decoder allocation failure, panic or watchdog was
observed. Model results were negative cat224 detections, not labeled screw events.
Neither trial meets a strict maximum 100 ms acquisition-gap requirement.

## Working decision

Use **one JPEG in flight as the next freshness-oriented fixed baseline**, and
retain two slots as the throughput comparison. This is a research baseline
selection, not a claim that one slot optimizes every QoS dimension. Keep slot
count explicit in build commands and trial configuration checks; the existing
default of two is preserved for reproducibility.

The next experiment should connect application capture identity/time to server
completion and specify image/event deadline curves. Do not turn this slot-time
measurement into a deadline-success claim. Before adopting a dynamic scheduler,
compare against this simple one-slot baseline using a replay trace with known
events. AI inference cost and acquisition jitter remain unresolved separately.

Artifacts: `results/device-slots1-v5/` and `results/device-slots2-v5/`, including
`outcome.json`, `manifest.json`, `pump-summary.json`, received JPEGs, all event
records, the exact firmware and flash/restore logs. Raw artifacts remain local
and ignored. The harness returns the board to its original FOMO firmware.

Final restoration check: five baseline settings acknowledged; seven FOMO AI
runtime samples on CPU0; no NVS save. Receiver port14568 is stopped. Both slot
configurations built and passed physical transport checks; the slot-order
regression passed ASan/UBSan. The normal operating firmware remains FOMO.
