# Temporary ESPDet on-device probe

Purpose: test the official 224x224 INT8 cat detector and real MCU crop/JPEG encoding before integrating a camera/network/VLM pipeline. Uses the official 640x480 two-cat JPEG, not screw training or ground-truth accuracy evaluation.

- Isolated ESP-IDF v5.3.6 / ESP-DL checkout, pinned commits and original model/image hashes in ../PROBE_MANIFEST.json.
- Existing 4 MiB partition layout and 0x1f0000 OTA slots, DIO flash, octal PSRAM at 40 MHz, CPU240 MHz. Does not rewrite bootloader, partition table, NVS or Wi-Fi credentials.
- Model stored in rodata; no model partition needed. Select only actual model operators using ../select_model_ops.py. This makes a documented local edit to the ignored vendor checkout. Unsupported pixel conversions are disabled; the tested path is RGB888.
- 3 warm-up +20 measured iterations. Each decodes source JPEG, runs detector (preprocessing + model + postprocessing), crops every detected box, encodes each ROI JPEG at quality80, frees temporary buffers, waits100ms. Constructor/model-load time is separate.
- ROI byte count is the sum of all cropped JPEGs; compare to original JPEG carefully because source JPEG quality/subsampling may differ. Includes neither transport framing nor class/box metadata.
- Crop timing includes coordinate clamping, allocation, row copy and JPEG encoding; excludes diagnostic printing, first-iteration validation, frees, and loop pacing. Sum of phase times is not measured network E2E latency. Original image decode is included separately.
- Crop bounds use clamped [x0,x1), [y0,y1). JPEG decode/dimensions are checked for each ROI on the first iteration. No semantic-fidelity or VLM-quality claim follows from successful decoding.
- Memory snapshots record phase free/largest heap and allocator low-water marks. Summed per-region low-water values are conservative allocator figures, not exact simultaneous peak RAM. Transient allocations can occur between snapshots.
- No camera/Wi-Fi/QUIC is started by this temporary firmware. Model may use both cores through ESP-DL. Neither this timing nor reciprocal FPS is concurrent live-camera throughput.
- At completion/failure it calls the standard OTA rollback API and returns to the previous valid firmware. It deliberately never marks itself valid. Host runner republishes the baseline release immediately on probe boot and verifies baseline core0 inference afterward.

Reproduction (dependencies must first be checked out at manifest commits and IDF tools installed in ../vendor/idf-tools):

```sh
python3 experiments/vlm_split/select_model_ops.py
bash experiments/vlm_split/build_probe.sh
python3 experiments/vlm_split/run_probe.py
python3 experiments/vlm_split/analyze_probe.py experiments/vlm_split/results/probe-TIMESTAMP
```

The runner uses the current project's existing local OTA service and known-good causal-core0-v25 baseline artifact. Do not use generic `idf.py flash` from the generated build instructions: that would write bootloader/partitions as well as the application. The runner checks the application fits the current slots and installs only through existing OTA.
