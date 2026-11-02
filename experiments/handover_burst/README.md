# Weak-link behavior on the ESP32

The user clarified that the target is weakening Wi-Fi, not increased object count or artificially slower CNN compute. The user authorized artificial impairment before a later physical experiment.

`weak_trial.py` compares policy1 (latest-frame admission) with policy3 (RSSI/RTT quality admission) on the existing FOMO device. It also checks legacy profile0 because its AI submission is coupled to successful image admission, unlike diagnostic profile5. No firmware flash or credential change is performed.

The dedicated `weak_shaper.py` IFB applies **256kbit/s, 200ms extra delay, 3% random loss, 128-packet queue** only to the verified ESP32-to-receiver UDP flow on receiver ingress. Existing root qdiscs are preserved. Cleanup removes the dedicated filter/interface and removes clsact only if no other filters remain. Normal completion, Python exceptions and SIGTERM/SIGINT restore the original runtime settings and remove shaping; abrupt power loss/SIGKILL still requires cleanup. Do not run concurrently with another serial client or network impairment experiment.

This is NOT a radio simulation: device RSSI, Wi-Fi PHY rate, MAC retries and RF handover are not controlled. It tests the effects of impaired end-to-end service capacity. The RTT branch of policy3 can react; the RSSI branch cannot be claimed tested by this impairment. Receiver-side pacing permits host-side queueing and does not reproduce identical device Wi-Fi CPU cost.

Each condition has a 5s analysis exclusion. Sequential live-camera conditions are not identical inputs, and a single sweep is characterization rather than a statistically replicated comparison. Recovery starts after removing the IFB (remaining emulated backlog can be discarded); it is not proof of natural RF recovery latency.

Offline analysis: `python experiments/handover_burst/analyze_weak.py RUN_DIRECTORY`. Requires Pillow for complete JPEG validation. Capture logs use serial observation times in legacy mode; diagnostic mode also provides device-time frame counters. Neither establishes a per-frame 100ms bound. Memory during diagnostic stages uses local serial samples; legacy memory comes from received telemetry and can be delayed.

`rssi_policy_probe.c` replays decision inputs through the actual admission source on the host. It is only a policy decision-path check, not ESP32/RF performance evidence.

The earlier `trial.py` full-outage/burst plan was interrupted following the clarification, before any blackhole was applied; settings were restored. It is not a completed handover result.
