# A–E active workload profiling

`profile 0` restores the existing camera/AI preview behavior. Runtime-only controls, no Wi-Fi/NVS changes:

| Command | Active workload |
|---|---|
| profile 1 | A: capture, no new AI offers or detection/image output |
| profile 2 | B: capture + independent local AI offers |
| profile 3 | C: B + MQTT QoS0 result metadata |
| profile 4 | D: C + source JPEG for each positive inference result |
| profile 5 | E: C + continuous preview attempts |

Model and input dimensions stay unchanged. AI offers retain the existing 5Hz ceiling and single pending latest-frame slot. Detection output remains bounded by the existing result representation (up to 3 detections). D is a per-positive-result workload, not a debounced physical-event implementation. False positives are allowed as workload triggers, not treated as correct detections.

For D, the worker retains the JPEG used for that result with matching capture timestamp; a later camera image is not substituted. Evidence uses the existing best-effort image DATAGRAM transport, including memory/admission/expiry constraints. C/D/E metadata uses actual MQTT over QUIC STREAM, unlike the old binary AI-result DATAGRAM. The legacy profile retains its old wire behavior.

All profiles keep Wi-Fi/QUIC connectivity, RAM telemetry, model/decoder resources, task stacks and 512KiB of common evidence-copy buffers. The tensor arena is lazily allocated at first inference and then retained: in the first run, first A was before this allocation while final A retained it. These A stages must be reported separately. The compiled person model uses a 384KiB arena, not the 1MiB default vehicle model asset. The new evidence buffers are reserved even outside D for a controlled common memory layout; this is implementation overhead, not an unavoidable cost of metadata-only AI. A measures camera-only activity with common reserved resources, **not a standalone camera build's minimum memory**. Switching to A can finish existing in-flight/pending inference; the runner excludes 15 seconds after switches.

UART emits 1Hz `[PROFILE]` rows with per-pool heap samples, cumulative capture/inference/output counts and IDLE0/IDLE1 task runtime counters. `[AI_PROFILE]` records each inference completion and measured wall duration around ai_inference_run, which includes preprocessing and scheduling effects. A 48-task static snapshot is used; tasks=0 makes CPU data invalid. CPU occupancy is estimated by subtracting per-core idle runtime fraction from 1, using unsigned 32-bit differences. It is not CNN-only CPU utilization.

The runtime-generated synthetic camera status/event workload and OTA polling are suppressed while profiling. Periodic RAM diagnostics stay enabled and are common overhead. Application profile control does not disable all network tasks.

Run with an attached USB console and the existing receiver:

```bash
python3 experiments/aiot_resource_management/profile_trial.py
python3 experiments/aiot_resource_management/analyze_profiles.py PATH_TO_RUN
```

The runner uses A→E→A order (E appears twice), 45s per stage, discards the first 15s and restores profile 0 plus initial video policy/settings. It stops on telemetry silence, reboot, internal free heap below 16KiB or PSRAM below 64KiB. Receiver MQTT records are counted directly; image chunk bytes are explicitly not complete-frame goodput. Live scenes are not identical sensor replay, and the two adjacent E stages are not independent replicates.

Firmware v22 adds these profiling controls and keeps the earlier deployment in `results/profile-firmware-v22/previous.bin`. The model paths must be passed explicitly to OTA build.sh, since its defaults point to a different older model:

```bash
source /home/ryzen395/esp-idf/export.sh
AIOT_CAMERA_MODEL_SOURCE="$PWD/experiments/person_detection/out/ai_inference_vehicle_model.c" \
AIOT_CAMERA_MODEL_METADATA="$PWD/experiments/person_detection/out/person_asset.h" \
bash experiments/ota/build.sh profile-v22
```
