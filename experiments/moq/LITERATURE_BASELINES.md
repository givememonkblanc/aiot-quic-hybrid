# MoQ literature-to-baseline map

Review cutoff: 2026-09-23. This map covers primary MoQ/MOQT specifications,
peer-reviewed systems papers, and directly relevant preprints that propose or
evaluate a transport, scheduling, timeout, quality-switching, semantic, or
multipath mechanism. Papers that merely use MoQ as an unchanged carrier are
listed as scope references but are not turned into scheduler baselines.

## What the literature actually compares

| Source | Mechanism and original comparison | Metrics used | AIoT adaptation in this project |
|---|---|---|---|
| [MOQT draft-21](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/21/) | Subscriber priority, publisher priority, group order, object/subgroup delivery timeout, and cancellation at resource limits | Protocol behavior rather than one evaluation KPI | `S5 moqt21_priority`: AI subscription first, importance within a subscription, descending preview group order. `T0` later adds post-submission timeout/reset. |
| [WARP Streaming Format](https://datatracker.ietf.org/doc/draft-ietf-moq-warp/00/) | Independently transferable media objects, catalog, real-time prioritization and ABR workflow | Live latency and playable media continuity | Keep event/evidence/preview as independent objects. A JPEG-only workload has no I/P-frame dependency, so WARP cannot be claimed as reproduced until an encoded dependent stream exists. |
| [Gurel et al., MMSys 2023](https://doi.org/10.1145/3587819.3593937) and [prioritized live sports follow-up](https://www.ibc.org/ibc-show/reports/a-fresh-look-at-live-sports-streaming-with-prioritized-media-over-quic-transport/22580) | MoQ testbed, passive bandwidth estimation including I-frame-average style estimation, and stream prioritization under bandwidth constraints | On-time display ratio, bandwidth estimate and low-latency playback | Compare fixed pacing/cap against measured-rate pacing; compare FIFO, draft priority and newest-group delivery. Do not call a JPEG average an I-frame estimator. |
| [Gurel et al., MMSys 2024](https://doi.org/10.1145/3625468.3652191) and [Nguyen et al., MMSys 2025](https://doi.org/10.1145/3712676.3719264) | MoQT versus LL-DASH testbeds with client-side ABR and live-edge behavior | Live latency, stalls/skips, bitrate and switching | Include a non-MoQ HTTP/QUIC or LL-DASH host reference only for a media-system comparison. It is not an MCU scheduler baseline and must use the same encoded representations. |
| [FrameTrace, EMS 2025](https://doi.org/10.1145/3746441.3748231) | Frame-level telemetry for MoQ instead of connection/segment averages | Per-frame latency and quality relationship | Preserve source/frame/event IDs through capture, inference, admission, QUIC ACK and receiver completion. Report run-level uncertainty rather than treating frames as independent runs. |
| [Freeman et al., point-cloud implicit adaptation](https://arxiv.org/abs/2507.15673) | Multiple descriptions on separate streams; delivery timeout implicitly sheds representations as the latency target tightens | Throughput, PCQM quality, and zero-representation stall rate | `T0`: delivery-timeout reset/drop. `MD0`: metadata/evidence base object plus optional preview/ROI enhancement objects; measure mission success, evidence validity and zero-evidence stalls. |
| [Distributed content filtering with MoQ](https://arxiv.org/abs/2505.08990) | Analysis result removes only objectionable segments and playback resumes afterward | Added latency and filtering behavior | Event-linked evidence admission: suppress invalid/non-mission media while preserving later objects. This is a semantic gating ablation, not proof of content moderation. |
| [Track Switching, ICMEW 2025](https://doi.org/10.1109/ICMEW68306.2025.11152184) and [time-shifted SWITCH evaluation](https://arxiv.org/abs/2606.26368) | Atomic switching among alternative tracks; live and time-shifted ABR under stable, step and sinusoidal capacity | Throughput, rebuffer ratio, switch latency and quality stability | `Q0`: low/high JPEG or ROI tracks with atomic epoch-aligned switching. Evaluate step and varying bandwidth; count evidence gaps and switch latency. |
| [MultiMoQ](https://arxiv.org/abs/2608.06102) | Multi-access base/enhancement tile scheduling and access switching versus standard MoQ and DASH | Base/enhancement goodput, tail E2E latency, stalls and continuity | Later dual-access experiment: evidence/base continuity first, preview enhancements second. Not a valid single-Wi-Fi baseline. |
| [MoMQ](https://arxiv.org/abs/2608.10741) | Object-metadata rules drive multipath choice; compared with single path, MinRTT, round-robin, BLEST and redundant scheduling | p50/p99/p99.9 frame completion, playback buffer and metered-path use | Later Wi-Fi/hotspot phase: event/evidence metadata and path state select paths. Reproduce transport-only path baselines before a semantic rule policy. Current `S4` is only a single-path semantic-order proxy. |
| [MD2G-Cast](https://arxiv.org/abs/2608.10020) | Relay-coordinated base multicast and selective enhancement admission for many viewers | p99 delivery interval, utility and aggregate load | Out of current one-publisher/one-subscriber scope. Retain as a relay/multi-client extension; do not add PPO to the ESP32 experiment. |

## Baselines that will be compared now

The first single-link comparison keeps identical objects, model, cipher,
memory budget and network trace.

| ID | Admission | Selector | Literature role |
|---|---|---|---|
| A0 | bounded FIFO/reject-new | strict AI first | retention baseline |
| S0 | latest pending | FIFO | no-priority baseline |
| S1 | latest pending | strict AI first | strongest simple project baseline |
| S2 | latest pending | byte DRR | fair-share baseline |
| S3 | latest pending | EDF | generic deadline baseline |
| S4 | latest pending | importance then EDF | single-path semantic proxy |
| S5 | latest pending | draft-21 priority, descending group | MOQT-native ordering baseline |

`S5` is implemented in `moqt_scheduler.c`. It is deliberately named a
project-profile adaptation: the current wire profile has two fixed
subscriptions and does not yet expose arbitrary subscriber priority updates.
With only one preview slot, descending group order degenerates to AI-first, so
the diagnostic must include a two-preview-slot case before claiming that group
order has been exercised.

### One-slot S1 versus S5 pilot

One bounded ESP32 run per policy used the same public screw model, UXGA JPEG,
32 KiB preview-unacknowledged cap, 128 kbit/s uplink, 50 ms added delay and a
64-packet shaper queue. This is a mechanism check, not a statistical comparison.

| Policy | Preview objects | Preview goodput | AI objects | AI capture→observed p95 | AI source→ACK p95 | Shaper drops |
|---|---:|---:|---:|---:|---:|---:|
| S1 strict AI-first | 10 | 105.0 kbit/s | 22 | 3.122 s | 5.257 s | 0/947 |
| S5 draft-21 priority | 9 | 102.3 kbit/s | 22 | 3.182 s | 5.321 s | 0/928 |

S5 showed no improvement in this one-slot pilot. That is the expected
degenerate case: both policies choose AI before the only eligible preview.
The small numerical differences are run variation and cannot establish a
ranking from one run. A two-slot microbenchmark is required to exercise
descending Group Order; the mission comparison still keeps one common slot so
slot-count effects do not contaminate policy effects. Machine-readable results
are in `results/draft21-priority-pilot-20260923.json`.

## Transport ablations after ordering

1. `T0 delivery-timeout`: the opt-in implementation now stops an expired
   admitted object and issues QUIC `RESET_STREAM` with draft-21
   `DELIVERY_TIMEOUT (0x2)`. Sender reset/discard counters and receiver
   reset-aware `PUBLISH_DONE` Stream Count accounting are implemented; host
   tests, an ESP32 build and one shaped device run pass. In the 128 kbit/s +
   50 ms mechanism run, the 2 s timeout reset all 15 admitted preview streams,
   delivered 0 complete previews and retained all 22 AI objects. Relative to
   timeout-off S1, link bytes fell only 2.6%, while AI capture-to-observed p95
   rose 3.3% and source-to-ACK p95 rose 0.6%. This setting is rejected: it
   violates the preview floor while repeated admissions keep the link busy;
   per-reset byte timing is required to isolate cancellation waste.
   No QoS benefit is claimed from one run. ngtcp2 in this build does not expose
   `RESET_STREAM_AT`;
   a reset received before the subgroup header is therefore logged as
   unattributed and invalidates exact per-track accounting instead of being
   silently assigned.
2. `MD0 base/enhancement`: AI metadata plus event evidence is the base mission
   object; full preview or ROI refinement is enhancement. Both use immutable
   event IDs and independent deadlines.
3. `Q0 track switch`: create frozen low/high-quality representations with
   aligned source epochs, then compare fixed-low, fixed-high, measured-rate
   switch and an offline oracle. Switching without alternative representations
   is meaningless.

## Later mobility/multipath baselines

Only after two live interfaces and path-level accounting work should the matrix
add single Wi-Fi, single hotspot, MinRTT, round-robin, BLEST-like blocking
avoidance, redundant sending, MultiMoQ-style base/enhancement steering and
MoMQ-style semantic rules. A host outage or AP reconnect is not a multipath
reproduction.

## Common outcomes

The primary denominator remains all ground-truth screw events. Success requires
a correct event and its required evidence before the declared deadline.
Secondary outcomes follow the papers where applicable: frame/object completion
time, receiver AoI, preview/evidence gap, stall or zero-evidence rate, quality,
goodput, switching latency, path use, CPU time, queue bytes and minimum heap.
Every comparison also reports misses and false positives so dropping all media
cannot win through latency alone.

The experiment order is `S0--S5/A0` ordering diagnosis, `T0` timeout ablation,
`MD0/Q0` representation adaptation, and finally multipath. This prevents a
large mixed matrix from hiding which literature mechanism caused an effect.

### T0 timeout mechanism pilot

Both runs used S1, one preview slot, the public screw model, UXGA JPEG, a
32 KiB unacknowledged-preview cap, 128 kbit/s uplink, 50 ms added delay and a
64-packet shaper queue. These are single runs, so the table validates the
mechanism and rejects one parameter setting rather than ranking policies.

| Timeout | Complete preview | Timeout resets | AI objects | AI capture→observed p95 | AI source→ACK p95 | Shaper bytes |
|---|---:|---:|---:|---:|---:|---:|
| OFF | 10 | 0 | 22 | 3.122 s | 5.257 s | 1,000,311 |
| ON, 2 s | 0 | 15 | 22 | 3.226 s | 5.288 s | 974,488 |

The result motivates feasibility-aware admission and smaller representations:
estimate whether an object can finish before its deadline before sending most
of it, select low-quality/ROI evidence when the full JPEG is infeasible, and
use reset only as an overrun guard. Machine-readable data are in
`results/t0-timeout-pilot-20260923.json`.

## Representation and evidence implementation status (v17--v20)

The implemented JPEG ladder uses one independent JPEG object per source group on
one media track. It tests application representation selection, not the atomic
Track Switching protocol. Fixed-high/medium/low baselines and a service-rate
controller share a frozen AI input. The v17 suite's raw traces, actual shaper
change times, frame hashes and per-run results are under
`results/ladder-v17-screen/`; an unfinished run never counts as completed.

A separate on-device codec and exact-source evidence mode have been built for
v18/v19. Source identity is checked by sequence AND capture timestamp before a
JPEG can be paired with an AI result. Original frames are not replaced with a
more recent unrelated image when a snapshot has expired. This implements the
base event/evidence relationship only; optional enhancement streams and full
multiple-description media remain unimplemented.

The original v17 controller incurred a cold-start timeout at 128 kbit/s. The
v20 variant starts at low quality before the first service observation. Treat
it as a new policy version; do not retroactively replace a bad v17 result.
