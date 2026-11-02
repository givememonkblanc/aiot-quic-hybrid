# 10Hz capture and CPU pipeline characterization — 2026-09-21

## Scope and conclusion

Current deployed FOMO/Core0 firmware sustained about 12.5 captured frames/s with live preview in this short run. This is capture throughput, not inference or delivered JPEG throughput. Setting the capture target to 10 instead of 30 reduced measured capture to 8.35Hz with preview, so nominal configuration is not an achieved rate.

A separate temporary ESPDet cat224/Core1 instrumented build localized gaps above 100ms to framebuffer availability at target30, and mostly main-loop dispatch at target10. Even camera-only had 17/420 inter-collection gaps above 100ms. Average >=10Hz must not be reported as a hard 100ms collection guarantee.

Image QUIC CPU work was about 346–350ms per 123179B frame. AEAD accounted for 123–129ms, with packetization excluding crypto accounting for 128–150ms. CPU cost is distributed, not just encryption. Current FOMO workload had substantial free PSRAM and a bounded queue; these measurements do not indicate OOM as the limiting mechanism.

## Method and model separation

- Existing FOMO/Core0: nine live-camera conditions, each 45s, first 10s excluded; 1024x1024 JPEG roughly 30–37KB. Scene was live, not byte-identical across conditions. Shared 256KiB evidence buffer was warmed before comparisons.
- Temporary ESPDet cat224/Core1: six 45s conditions, first 10s excluded. AI priority1 and TX priority5. Isolated IDF5.3.6, software ChaCha20-Poly1305, hardware AES off; encryption and watchdog remained enabled.
- First two ESPDet conditions replay the same 123179B 1024x1024 JPEG for both AI and TX. Replay generation rate is not sensor capture rate. Last four use the real camera.
- One characterization sweep, not a replicated inferential comparison. Probe overhead is included; precise small differences between 1KiB/8KiB are not statistically established.
- Original complete 8MiB flash restored and esptool hash verified. CLI ACKs verify profile0/probe0/mode2/policy1/video30 budget6144KiB; 20 subsequent AI samples report Core0. No NVS save, network shaping or receiver replacement.

## Deployed FOMO resource profile

CPU percentages below are per core. AI wall and CPU measurements cover the instrumented AI pipeline, including decode/preprocessing, rather than only CNN operators.

| Condition | Target | Capture Hz | AI wall ms | AI CPU ms | CPU0 / CPU1 % | Complete JPEG RX fps | RX KiB/s | Queue peak B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| camera-only-headroom | 30 | 12.50 | — | — | 10.52 / 0.19 | 0.00 | 0.00 | 0 |
| camera-FOMO | 30 | 12.50 | 741.84 | 328.15 | 54.67 / 0.20 | 0.00 | 0.00 | 0 |
| camera-FOMO-metadata | 30 | 12.50 | 741.96 | 325.49 | 55.10 / 0.24 | 0.00 | 0.00 | 0 |
| camera-FOMO-copy-drop | 30 | 12.53 | 744.84 | 325.27 | 55.11 / 0.30 | 0.00 | 0.00 | 0 |
| camera-FOMO-preview-1Hz | 30 | 12.53 | 893.44 | 346.62 | 60.33 / 20.24 | 0.94 | 31.99 | 30588 |
| camera-FOMO-preview-unlimited | 30 | 12.50 | 1595.18 | 442.89 | 72.15 / 64.33 | 3.11 | 105.40 | 33698 |
| capture-target10-preview | 10 | 8.35 | 1484.01 | 425.42 | 70.97 / 59.26 | 2.91 | 98.60 | 31553 |
| capture-target10-no-preview | 10 | 9.06 | 767.67 | 326.76 | 56.34 / 0.49 | 0.00 | 0.00 | 0 |
| camera-FOMO-preview-repeat | 30 | 12.50 | 1711.16 | 456.93 | 73.16 / 67.16 | 3.08 | 109.87 | 36243 |

At target30 all observed 1s/5s/10s count windows remained >=10Hz (allowing timestamp rounding). This firmware exposed count snapshots, not per-frame collection timestamps, so no hard inter-frame bound is established for deployed FOMO.

FOMO+metadata to FOMO+unrestricted preview changed AI CPU share from 42.8% to 27.4% of one core, main from 8.3% to 28.2%, net_tx from approximately 0% to 57.0%, Wi-Fi from 0.5% to 10.6%, and lwIP tiT from 0.2% to 7.1%. AI is on Core0 and net_tx on Core1: this is not evidence of direct same-core AI/net_tx competition. Main/Wi-Fi activity, synchronization and shared resources remain relevant. Wall minus CPU includes blocked and preempted time; it does not uniquely identify mutex delay or PSRAM stalls.

Internal free heap stayed roughly 52.6–56KB and free PSRAM roughly 5.40–5.44MB. Queue peaks are sampled, not allocation-exact maxima. Reported tx_failed/tx_expired counter deltas were zero; this does not count frames intentionally excluded by admission or prove loss-free Wi-Fi. Captured frames are not all admitted for transmission.

## QUIC 350ms decomposition: fixed bytes, separate ESPDet build

Values below are CPU milliseconds normalized by 121 submitted application fragments per frame. Frame-boundary truncation can cause small normalization error. The exclusive CPU rows sum to the image batch CPU cost; UDP socket CPU is nested within UDP helper cost and must not be added again.

| Exclusive CPU scope | 1KiB submission | 8KiB submission |
|---|---:|---:|
| AEAD encryption | 128.57 | 123.11 |
| Header protection | 10.59 | 10.59 |
| ngtcp2 packetization excluding crypto | 127.74 | 149.94 |
| UDP submit helper including socket | 39.25 | 38.89 |
| Other image batch processing | 39.35 | 27.28 |
| Total | 345.50 | 349.82 |

| Additional scope (not additive to CPU table) | 1KiB | 8KiB |
|---|---:|---:|
| Connection lock acquisition wall ms/frame | 341.17 | 205.98 |
| Socket send wall ms/frame | 89.91 | 91.45 |
| Socket send CPU ms/frame | 34.70 | 34.08 |
| Application fragmentation CPU ms/frame | 26.39 | 27.48 |
| Application fragmentation wall ms/frame | 34.89 | 38.87 |
| AI pipeline wall ms | 2434.38 | 2685.21 |
| Complete JPEG goodput KiB/s | 116.53 | 154.62 |

Application fragmentation includes allocation, copying, 12B headers and queue enqueue; only fully admitted frames have FRAGMENT_PROFILE records. It is distinct from QUIC packetization. Each application DATAGRAM carries at most 1024 JPEG bytes plus 12B header; 8KiB batches still generate small DATAGRAMs/QUIC packets, not an 8KiB DATAGRAM. QUIC packet counts equalled fragment counts in these windows. A 20ms lock-hold budget remains active.

The ~350ms scope excludes application fragmentation, general QUIC receive/ACK/timer processing, Wi-Fi task CPU and camera/AI work. CPU1 was ~99.4–99.6% occupied under this ESPDet load. Main-thread QUIC activity also remains significant. Socket wall minus CPU (~55–57ms/frame) is aggregate off-CPU time inside submit, not a pure lwIP or Wi-Fi queue-time estimate. Connection-lock wall time is measured separately and falls with batching; it is not CPU computation.

Wi-Fi TXdone observer registered successfully but matched ciphertext fingerprints for only 3.17% / 6.61% of submitted packets. Matched-only submit-to-callback means were 9.38ms / 8.17ms. These are NOT representative full-path Wi-Fi latency estimates. Callback matching/coverage must be repaired before attributing delay to Wi-Fi queues, airtime or retries. Unmatched packets are not measured loss. The bounded pending table also overwrote unmatched records. TX completion is not receiver application delivery.

The task runtime counter includes the currently running slice. Nested probe overhead is included; the small header-protection CPU values can slightly exceed its separately bracketed wall measurements due to probe boundary overhead. No claim is made of cycle-perfect exclusive crypto profiling.

## Per-frame camera timing: instrumented ESPDet build

| Condition | Collection Hz | Gap p95 ms | Gap max ms | Gaps >100ms / observed | FB-wait dominant / dispatch dominant |
|---|---:|---:|---:|---:|---:|
| live-camera-only | 12.02 | 80.58 | 159.92 | 17 / 420 | 17 / 0 |
| live-AI-metadata | 11.48 | 159.70 | 239.87 | 33 / 401 | 33 / 0 |
| live-AI-preview | 11.62 | 156.38 | 237.98 | 30 / 406 | 29 / 1 |
| live-target10-AI-preview | 9.30 | 139.38 | 224.57 | 232 / 325 | 2 / 230 |

Collection is timestamped at JPEG copy completion. Dominant means that component accounts for >50% of a long gap; not all gaps must have a dominant component. Camera-only sensor-start to buffer-ready averaged 79.88ms, fb_get CPU 0.17ms and JPEG copy CPU 2.92ms. Sensor/DMA/JPEG completion overlaps fb_get waiting and must not be summed as serial phases. OV2640 generates JPEG; no separate MCU JPEG encoding time was isolated.

At target30, long gaps are mostly spent waiting for the next valid framebuffer, including gaps in camera-only. This localizes the delay but does not distinguish sensor cadence, DMA/frame validation, driver buffering or turnaround. Under AI+preview, copy CPU rose to ~6.51ms and copy wall to ~8.44ms, leaving shared-memory/cache effects among the candidates. At target10, dispatch averaged 95.91ms and accounted for 230 of 232 long gaps, consistent with the existing relative-period main-loop pacing under-running the configured target. Exact FOMO gap behavior still requires probes in that baseline build; these numbers cannot be silently transferred across firmware/models.

## Next engineering steps for the 10Hz QoS requirement

1. Define both throughput (>=10 samples/s over agreed windows) and deadline/jitter (e.g. inter-collection gap <=100ms or an explicitly allowed miss rate). Also measure age from sensor start; buffer reuse must not masquerade as a new sample.
2. Instrument camera VSYNC, valid/invalid frame and buffer-return sequence to explain the occasional 160–240ms framebuffer wait. Camera-only already exhibits this failure, so QUIC optimization alone cannot guarantee the capture deadline.
3. Move capture cadence away from relative main-loop pacing toward an absolute periodic schedule or dedicated capture task, then rerun the same profiles. This fix is proposed, not implemented in this measurement change.
4. Bound preview admission using residual capacity while preserving the capture and AI/event budgets. Batch submission helps lock waiting but does not remove per-byte crypto/packetization work.
5. Repair Wi-Fi callback correlation; report socket blocking, Wi-Fi completion and receiver goodput independently. Do not publish unmeasured airtime/queue attribution.

No detection accuracy/GT result or deadline-success claim is established by this profiling. A quick host resource sample was mostly idle, but it is not a synchronized whole-run host CPU profile.

## Artifacts

- `results/live-20260921T004340Z/{outcome,summary}.json`: deployed FOMO sweep.
- `results/pipeline-20260921T005217Z/{outcome,pipeline-summary,summary}.json`: temporary ESPDet scopes and receiver goodput.
- Both run directories retain records.jsonl, serial.log and captured receiver feed for offline inspection. Raw logs stay ignored/private because boot logs may contain configuration details.
- `results/restore-verification.json`, `results/restore-flash.log`: full baseline restoration and runtime ACKs.
- `results/pipeline-build-manifest.json`: instrumented artifacts and source hashes.
