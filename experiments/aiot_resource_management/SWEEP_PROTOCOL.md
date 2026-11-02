# Fixed JPEG core and priority diagnostic

Purpose: test whether ordinary static core/priority tuning removes concurrent image-TX interference at one fixed offered workload. This is not yet a dynamic scheduler evaluation or a transport comparison.

- Device: ESP32-S3, existing person FOMO model, 96x96 input; camera remains initialized on CPU0.
- Fixture: fixtures/replay.jpg, 145989 bytes, 1024x1024. SHA256 recorded in fixtures/manifest.json. One frozen real camera JPEG, repeatedly supplied from an immutable PSRAM copy (145989 additional bytes common to every cell).
- Nominal replay schedule: 10 frames/s; one image transmission attempt in each nominal second; no catch-up bursts. Missed input slots are outcomes. Actual offered frames and admitted network bytes are reported, not assumed equal.
- Application capture polling target 30 Hz. Replay bypasses application camera acquisition/copy; sensor/camera background tasks remain active. Replay FPS is not physical camera FPS.
- Existing AI admission: at most 5 offers/s, one pending frame replaced by a newer offer, one active frame. Log offers, overwrites, starts and completions.
- Profile 5: local AI + MQTT detection metadata + image preview; QUIC DATAGRAM images and MQTT/QUIC STREAM metadata. Adaptive image admission unchanged; queue budget 6144 KiB, mode2/policy1. No network shaping.
- Core0 order priorities 0,1,5,6; core1 reverse order 6,5,1,0. TX pinned core1/priority5. Main/camera/Wi-Fi retain original placement. Desired AI priority applies at next inference start; effective priority is checked.
- 90 seconds per cell, initial 15 seconds excluded. Descriptive p50/p95/p99 with completion count; insufficient for a tail guarantee. One pass is exploratory, with firmware/reboot and order confounds.
- No application deadline or minimum required goodput has been supplied. Predefined characterization grid: source-frame-to-inference-completion deadlines 1/2/3/5 seconds, complete-image goodput thresholds 32/64/96 KiB/s. This is not event deadline success, ground-truth accuracy, or an application guarantee.
- Source age is measured entirely on the MCU clock, from ideal replay timestamp to completion. Completed-job fractions are conditional on completion; overwritten/unserved frames are reported separately.
- Goodput counts complete, contiguous JPEGs matching the fixture SHA256, using receiver monotonic time. Partial/corrupt JPEGs do not contribute. Complete-image counts and accepted-image counts refer to their respective observation windows and are not a cohort packet-loss estimate.
- CPU0/1 load from idle-runtime deltas; task percentages use one-core capacity. Wall minus task CPU time is not a direct measurement of scheduler waiting alone.
- Abort on watchdog/panic, reboot during a stage, low heap or absent profiling telemetry. Retain failure as an outcome; do not disable watchdog.
- Restore replay off, AI priority0, profile0/probe0 and original core0 firmware after the sweep.

Interpretation: a static setting that succeeds here weakens the need for a new scheduler at this load. Failure of all settings does not imply a dynamic manager can meet impossible capacity demands. Neither outcome establishes behavior over a wider workload range.
