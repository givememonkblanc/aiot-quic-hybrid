# Weak-link artificial impairment: ESP32 FOMO — 2026-09-21

## Outcome

This completed nine-stage sweep found **service degradation before memory failure**. No watchdog, panic, reset or heap-guard abort was observed. Capture counters remained about 12.5Hz, while complete JPEG reception collapsed under impairment. In legacy profile0 with quality policy3, capture was 12.53Hz, AI pipeline mean 0.859s, and complete JPEG delivery 0.111FPS during the weak phase. The receiver observed 43 AI-result DATAGRAMs and the device logged 43 AI completions in that analysis window; equal counts do not prove per-event delivery success or deadlines.

This does not establish survival under all weak Wi-Fi conditions, true AP handover, long outages, or longer runs. User clarified that “burst” refers to link weakening/backlog effects, not increasing object count or artificially changing CNN compute.

## Controlled conditions

Existing deployed FOMO/Core0 firmware, live 1024x1024 camera JPEG (received legacy images about 35KB), target30, mode2, budget6144KiB. Profile5 independently submits camera frames to AI and sends preview plus MQTT diagnostic detection metadata. Profile0 is the existing normal application and sends AI results on QUIC DATAGRAM track2. These delivery paths are different and must not be conflated.

Only the verified ESP32 -> dedicated receiver UDP flow was redirected to a dedicated IFB. Impairment: **256kbit/s, 200ms added delay, 3% random packet loss, queue limit128 packets**. Receiver root qdiscs were preserved. The reverse direction was not directly shaped. Loaded RTT exceeded the configured added delay because of queueing/congestion; delay200ms does not mean RTT200ms.

This is an end-to-end capacity/delay/loss experiment, **not RSSI reduction or RF simulation**. PHY rate, MAC retries, association and real Wi-Fi driver CPU costs were not reproduced. RSSI was not injected into the ESP32. Policy3 reacts through its RTT branch in this experiment. Actual lower-RSSI validation remains for the user's physical experiment.

Each group ran good30s -> weak50s -> recovery25s, excluding the first5s of each stage from aggregate statistics. Policies were sequential, not randomized or byte-identical traces; random loss varied. One sweep does not establish statistical superiority. Removing IFB can discard its remaining host backlog; recovery is not natural RF recovery timing.

## Measurements

AI milliseconds below cover the instrumented AI pipeline (decode/preprocessing plus inference), not pure CNN kernel time. Capture is device frame-counter throughput, not a hard100ms inter-frame guarantee.

| Mode / policy | Link | Capture Hz | AI completions/s | AI wall mean ms | Complete JPEG RX FPS | AI results received | Min internal free B | Sampled queue peak B |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| latest-good | unshaped | 12.54 | 0.56 | 1697.7 | 3.113 | 14 | 51335 | 32310 |
| latest-weak | impaired | 12.50 | 1.07 | 923.9 | 0.133 | 49 | 46755 | 36576 |
| latest-recovery | unshaped | 12.53 | 0.60 | 1683.3 | 3.137 | 12 | 46663 | 31868 |
| quality-good | unshaped | 12.50 | 0.60 | 1651.1 | 3.118 | 15 | 45743 | 35627 |
| quality-weak | impaired | 12.50 | 1.04 | 923.5 | 0.311 | 49 | 45743 | 33484 |
| quality-recovery | unshaped | 12.47 | 0.60 | 1659.2 | 3.098 | 12 | 45755 | 34410 |
| legacy-quality-good | unshaped | 12.60 | 0.56 | 1673.7 | 3.116 | 14 | 45959 | 34857 |
| legacy-quality-weak | impaired | 12.53 | 0.95 | 859.0 | 0.111 | 43 | 45239 | 32605 |
| legacy-quality-recovery | unshaped | 12.40 | 0.60 | 1698.6 | 3.035 | 12 | 44623 | 35489 |

Result counts cover unequal25/45/20s analysis durations. They are not directly comparable as raw counts across stages, and may include work started before a boundary. No GT correctness or event deadline-success ratio is claimed. Profile5 counts MQTT diagnostic publications; profile0 counts distinct received track2 DATAGRAM identities.

Profile5 uses local1s memory/frame samples. Profile0 uses received5s RAM telemetry with device uptime counters; delayed delivery can shift phase attribution, so its values are approximate window measurements. Standard firmware suppresses per-frame camera logs, so serial per-frame timestamps were unavailable. Stage minima are sampled, not true allocation-level extrema. Heap lifetime low-water marks are not stage-specific.

## What failed and what did not

- Diagnostic policy1: complete JPEG reception fell3.113 ->0.133FPS; queue-expired fragment counter delta1088 during the analyzed weak window.
- Diagnostic policy3: complete JPEG reception fell3.118 ->0.311FPS; expired fragment delta156. This is a promising single-run difference, not proof of superiority. RTT median was1622ms for policy1 and2104ms for policy3; not all QoS dimensions improved.
- Normal application with policy3: complete JPEG reception fell3.116 ->0.111FPS; expired fragment delta525 and failed-fragment delta25. These counters refer to fragment operations, not lost whole frames or necessarily Wi-Fi losses. Complete JPEG goodput during weakness was3.87KiB/s despite the32KB/s configured link rate: incomplete JPEG fragments still consume capacity.
- Normal application's AI mean wall time fell1.674 ->0.859s and completion rate rose0.56 ->0.95Hz. Under this workload, reduced transmission work coincided with better AI timing. The test does not isolate Wi-Fi driver contention from shared CPU/memory effects.
- All stages had substantial free PSRAM (at least about5.37MB), internal free heap stayed above44KB, and sampled image queue peak stayed below37KB. No runaway buffering was observed. No long-term leak claim can be made from this sweep.
- Recovery analysis windows returned complete JPEG RX to about3.0–3.14FPS. This is recovery-window throughput, not a measured worst-case recovery deadline.

## Existing adaptation and gaps

Source `video_admission.c`: policy1 gates queue occupancy and inflight proxy but does not throttle directly on weak RSSI; policy3 adds RSSI EWMA<=-75dBm or RTT EWMA>180ms and limits weak-link image admission to at most one frame/s after activation. Recovery uses RSSI>-67dBm and RTT<=150ms with hysteresis. These thresholds are implementation settings, not universally validated Wi-Fi quality boundaries.

`rssi_policy_probe.c` separately exercised this source on the host with a healthy -> -82dBm -> healthy input sequence and empty queues. During the10s weak phase, policy1 admitted100/100 attempts, while policies2/3 admitted19/100 (includes detection transition). This is decision-path evidence only, not ESP32/RF performance.

The current source's legacy path submits AI after successful image admission. This is a possible coupling risk at worse capacity or faster models, but **AI starvation did not occur in this measured weak condition**. Do not convert a source-level risk into an observed failure. There is no implemented joint AI deadline/resource guarantee established by these tests.

At35KB/JPEG and10Hz, image payload alone is about2.8Mbit/s, roughly11x the256kbit/s limit. Preserving local capture10Hz does not imply shipping all captured full images10Hz. The next QoS target must specify which data must reach the server: AI events, selected evidence or every full image. Full-image delivery requires lower bytes/rate or greater capacity; scheduler changes alone cannot overcome this byte budget.

## Next validation

1. Repeat fixed input trials and add explicit event IDs/deadlines/GT so service success and dropped events are scored, not just received counts.
2. Test adaptive image size/ROI/rate with frame-level expiry, while keeping local acquisition independent of image admission; evaluate that change separately from this measurement-only work.
3. Physical weak-Wi-Fi sweep: log measured RSSI, RTT, complete goodput, Wi-Fi retries if exposed, per-core runtime and collection timing, then return to strong signal. Compare at similar delivered capacity; netem does not reproduce RF retry CPU load.
4. Include actual AP transition separately. No AP handover was induced here.

## Completion and artifacts

- Run: `results/weak-20260921T024141Z`, with raw serial, structured records, receiver feed, outcome, summary and weak-summary JSON.
- Every stage completed. Outcome contains qdisc counters demonstrating active shaping and its drops.
- Dedicated IFB and filter removed; absence verified. Baseline ACKs restored profile0/probe0/mode2/policy1/target30/budget6144KiB. No flash, NVS save, server restart or credential change.
- Earlier full-blackhole plan was stopped after the user clarified scope, before any block was applied; it is not handover evidence.
