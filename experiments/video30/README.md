# 30 FPS video investigation

Goal: distinct, fully reassembled JPEG frames received at 30 FPS while retaining AI functionality. Capture rate, completed-image rate, and inference rate are separate metrics. The camera remains at the existing 1024×1024 setting; no resolution or quality reduction has been applied.

Implementation under test:

- Frame admission interval 41→33 ms; application loop delay 20→1 ms and QUIC process delay 5→1 tick.
- TX yields after a bounded 3 ms work interval rather than sleeping after each fragment. Congestion control, frame admission, expiry and watchdog remain enabled.
- Per-second `DGRAM_PERF` separates lock wait, packet-generation/encryption wall time and UDP send time. It does not isolate AES alone or CPU execution from preemption.
- Opt-in `AIOT_QUIC_PSRAM_POOL` moves ngtcp2 allocations >=512 B and TLS allocations >=256 B to PSRAM. The TLS `XMALLOC_USER` hooks are necessary: FreeRTOS's default `XMALLOC → pvPortMalloc` bypasses `wolfSSL_SetAllocators`. malloc/free/realloc use matched hooks.
- Configurable TLS 1.3 cipher list permits a ChaCha20-Poly1305 experiment. Existing certificate policy is unchanged.
- AES-GCM known-answer encrypt/decrypt and tampered-tag rejection checks run before transport initialization. The hardware-AES + 32-bit GHASH experiment failed this check and was rejected; tested subsequent builds keep hardware AES disabled. Failure does not isolate which change caused the incorrect result.
- Temporary ESPDet boot starts in camera-only profile1 so model initialization does not race the initial handshake.
- ROI RGB input allocation now explicitly meets the JPEG encoder's 16-byte alignment requirement.
- The final experimental main stack is 12 KiB rather than 20 KiB: prior measured minimum free stack was 11,928 B (8,552 B peak use). Stack telemetry is collected for further validation. This is not a general proof of worst-case stack safety.

`build.sh` uses the isolated ESPDet integration environment and temporary 8 MiB partition configuration. `trial.py` logs raw serial/receiver data and stops at internal free heap <16 KiB, PSRAM <128 KiB, reboot/watchdog, or missing telemetry. The limit has not been relaxed to complete a run. `summarize.py RUN_DIR` derives completed JPEG FPS and packet timings.

Early failed/aborted runs are retained in `results/`. TLS failures are invalid throughput measurements. Runs stopped before the initial 10-second settling interval do not supply steady-state throughput comparisons. Camera-only measurements during failed TLS reconnects are not a pure sensor-limit benchmark.

The fixed fixture is the official 64,065-byte cat JPEG. At 30 FPS it requires 1,921,950 payload B/s (15.38 Mbit/s), before ROI, metadata and protocol overhead. This arithmetic is a workload requirement, not an ESP32 capability claim. Live-camera images may have different sizes.

No server VLM, screw accuracy, energy or transport-vs-TCP comparison is included. See RESULTS.md for measured results and device restoration status.
