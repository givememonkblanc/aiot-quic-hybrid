# Fixed JPEG core × priority results

Each cell: 90 seconds, first 15 seconds excluded. Fixed 145989-byte JPEG, nominal 10 Hz replay and 1 Hz image attempt. Existing person FOMO; this is not TinyVLM. TX core1/priority5. No added network impairment.

| AI core | Priority | N | AI mean / p95 / p99 (s) | Source-to-finish p95 (s) | Complete JPEG KiB/s | Replay FPS | Missed input slots | CPU0 / CPU1 % |
|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 29 | 2.49 / 2.77 / 2.93 | 3.12 | 72.2 | 10.01 | 0 | 75.37 / 45.09 |
| 0 | 1 | 61 | 1.18 / 1.52 / 1.53 | 1.72 | 72.2 | 9.54 | 35 | 99.14 / 47.77 |
| 0 | 5 | 95 | 0.67 / 0.94 / 0.94 | 1.03 | 15.2 | 1.27 | 660 | 96.10 / 26.47 |
| 0 | 6 | 96 | 0.66 / 0.92 / 0.94 | 1.04 | 13.3 | 1.28 | 652 | 95.87 / 27.62 |
| 1 | 6 | 124 | 0.55 / 0.58 / 0.58 | 0.84 | 0.0 | 3.80 | 466 | 53.56 / 95.13 |
| 1 | 5 | 89 | 0.80 / 1.00 / 1.01 | 1.32 | 0.0 | 10.01 | 0 | 48.11 / 98.38 |
| 1 | 1 | 66 | 1.06 / 1.51 / 1.54 | 1.70 | 70.3 | 10.00 | 0 | 54.63 / 98.44 |
| 1 | 0 | 34 | 2.09 / 2.40 / 2.42 | 2.71 | 70.3 | 10.00 | 0 | 51.46 / 70.77 |

## Interpretation

At this offered workload, core1/priority1 is a promising static configuration: mean AI 1.06s, complete-image goodput 70.3 KiB/s, no nominal replay-slot misses. Core0/priority1 also preserves goodput while reducing inference time, but misses some input slots. This weakens the case for a new dynamic manager at this load; it does not prove robustness across loads or repeated runs.
Raising priority to 5/6 trades away service: core0 input supply falls to about 1.3 FPS and image goodput falls; core1 yields zero verified complete JPEGs despite faster inference. Completion latency alone would select misleading settings. The exact transport/scheduler wait mechanism remains uninstrumented.


## Delivery and memory

| Core / priority | Image attempts / rejected | Accepted / complete cohort | AI offers / overwritten | MQTT results received | Peak queue KiB | Peak PSRAM MiB | Min internal free KiB |
|---|---|---|---|---|---|---|---|
| 0/0 | 75/38 | 37/37 | 280/251 | 29 | 63.24 | 3.01 | 52.73 |
| 0/1 | 75/38 | 37/37 | 281/220 | 61 | 67.25 | 3.02 | 44.32 |
| 0/5 | 72/41 | 31/9 | 94/0 | 89 | 144.55 | 3.09 | 38.08 |
| 0/6 | 73/43 | 30/7 | 96/0 | 93 | 139.53 | 3.12 | 45.98 |
| 1/6 | 51/26 | 25/0 | 158/34 | 52 | 146.56 | 3.09 | 43.32 |
| 1/5 | 75/50 | 25/0 | 300/212 | 89 | 126.48 | 3.07 | 52.66 |
| 1/1 | 75/37 | 38/38 | 298/231 | 67 | 97.37 | 3.05 | 52.66 |
| 1/0 | 75/37 | 38/38 | 296/261 | 35 | 95.36 | 3.04 | 52.47 |

Complete cohort: JPEG accepted in the observation window and received by window end +8 seconds; contiguous bytes and SHA256 must match. Outstanding/noncomplete frames are not labelled network packet loss. Admission rejects, incomplete accepted images, AI pending replacements and nominal input misses are separate.

## Characterization grid

Cells list static configurations with all observed completed jobs within the source-to-finish deadline AND measured complete-image goodput above the threshold. No application requirement was supplied. These are descriptive pass cells, not guarantees. They do not require all nominal inputs to be processed.

| Deadline | ≥32 KiB/s | ≥64 KiB/s | ≥96 KiB/s |
|---|---|---|---|
| 1s | none observed | none observed | none observed |
| 2s | C0/P1, C1/P1 | C0/P1, C1/P1 | none observed |
| 3s | C0/P1, C1/P1, C1/P0 | C0/P1, C1/P1, C1/P0 | none observed |
| 5s | C0/P0, C0/P1, C1/P1, C1/P0 | C0/P0, C0/P1, C1/P1, C1/P0 | none observed |

## Task runtime (% of one core)

| Core / priority | AI | main | camera | TX | Wi-Fi |
|---|---|---|---|---|---|
| 0/0 | 24.12 | 26.19 | 16.58 | 39.56 | 7.09 |
| 0/1 | 50.58 | 23.87 | 16.19 | 42.15 | 7.13 |
| 0/5 | 70.46 | 6.76 | 13.99 | 22.92 | 4.28 |
| 0/6 | 71.49 | 5.62 | 13.52 | 24.91 | 4.63 |
| 1/6 | 92.47 | 38.57 | 12.93 | 2.22 | 0.90 |
| 1/5 | 71.84 | 26.88 | 14.95 | 22.99 | 4.81 |
| 1/1 | 55.56 | 29.53 | 15.97 | 37.52 | 7.67 |
| 1/0 | 28.34 | 27.40 | 15.31 | 37.10 | 7.32 |

## Limits

- Descriptive tails from a single short pass; report N, do not treat p99 as a validated tail bound or observed maximum as WCET.
- Single fixed frame, no object/event ground truth: no F1, recall or end-to-end event deadline success can be claimed.
- Replay FPS measures supplied fixture frames, not physical capture FPS. Camera background processing remains enabled; app camera acquisition/copy is bypassed. Main starvation reduces actual supplied frames despite the same nominal schedule.
- Completed-job latency may improve while input slots or pending frames are dropped. Evaluate these jointly.
- Core runs require different firmware/reboots; reversed priority order mitigates but does not remove temporal/network/order confounds. No repeats here.
- Aggregate CPU cost per complete-image byte includes AI and common overhead; it is a system cost, not isolated QUIC cost.
- Success at one static workload does not cover the whole workload space. Failure here does not establish that dynamic control can overcome capacity limits.
- See SWEEP_PROTOCOL.md and machine-readable sweep_summary.json for detailed metrics, per-task priorities, conditional deadline fractions and firmware manifests.

## Raw runs

- [sweep-core0-20260920T152041Z](results/sweep-core0-20260920T152041Z/outcome.json)
- [sweep-core1-20260920T152908Z](results/sweep-core1-20260920T152908Z/outcome.json)

## Restoration

Original causal-core0-v25 firmware restored via OTA (SHA256 797cd9ed7d44a4cba2fab53175f3e47b7063d40912671e4277e13f23a47ea13b). Serial inference records confirm core0; original firmware has no replay path and AI priority defaults to0. Profile0/probe0/mode2/policy1/target30/budget6144 KiB ACKs and fresh receiver memory telemetry were verified. No network shaping was applied. Local build configuration restored to AI core0. TinyVLM was not deployed because the requested pretrained artifacts were unavailable; see ../tinyvlm/PORT_STATUS.md.
