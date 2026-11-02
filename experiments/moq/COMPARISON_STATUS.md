# Mission-Based AIoT Media over QUIC: comparison status

Updated: 2026-09-23.

## What can be concluded now

The repeated fixed-input diagnostic supports `latest + strict AI-first (S1)` as
the strongest simple baseline. Under the one 512 kbit/s + 50 ms run, S1 reduced
AI source-to-ACK p95 from 4.661 s (FIFO/S0) to 3.541 s while delivering all 27
AI results. Equal-byte DRR/S2 reached 3.602 s and also delivered all 27. These
are transport/service observations without screw ground truth, not a claim of
better detection accuracy or a statistically established paper result.

One-slot draft-21 ordering (S5) produced no measurable advantage over S1 in its
single 128 kbit/s pilot: both selectors choose AI before the only eligible
preview. S5 must be exercised with two pending preview groups to test descending
Group Order, while the final mission matrix must keep the same slot budget for
all policies.

The first paired `T0` mechanism run also rejected the current timeout setting.
At 128 kbit/s + 50 ms, a 2 s post-admission timeout reset 15/15 admitted preview
streams and delivered no complete preview, while all 22 AI objects completed.
Compared with timeout-off S1, shaper bytes fell only 2.6%, AI
capture-to-observed p95 rose 3.3%, and AI source-to-ACK p95 rose 0.6%. Thus the
reset path works, but this setting violates the preview floor. Similar total
link bytes do not isolate the cause: bytes already submitted and repeated
admission of new oversized objects can both keep the link busy.

## Comparable mechanisms

| ID | Mechanism | Implementation | Device evidence | Current interpretation |
|---|---|---|---|---|
| A0 | bounded FIFO retention + AI-first | complete | repeated unshaped | preserves more old previews but worsens freshness |
| S0 | FIFO selection | complete | repeated + one degraded | delays AI behind preview bytes |
| S1 | latest + strict AI-first | complete | repeated + degraded pilots | simple baseline to beat |
| S2 | byte DRR | complete | repeated + degraded pilots | fairness did not beat S1 in current workload |
| S3 | EDF | complete | repeated unshaped | earlier preview deadline delays AI |
| S4 | importance then EDF | complete | repeated unshaped | collapses near S1 when AI importance is constant |
| S5 | draft-21 priority/group order | complete | one-slot pilot | degenerates to S1 with one preview slot |
| T0 | post-admission delivery-timeout reset | complete | one degraded paired pilot | 2 s/UXGA setting loses every preview with only 2.6% fewer link bytes; parameterization rejected |
| MD0 | base event/evidence + optional enhancement | specified | not implemented | needed for mission-aware quality trade-off |
| Q0 | atomic alternative-track switching | specified | not implemented | cannot be inferred from varying JPEG dimensions on one track |
| QJ0 | same-source per-object JPEG representation selection | implemented | v17 screening in progress | compare against all fixed representations; no QoS superiority assumed |
| E0 | exact-source AI result/evidence pairing | implemented/build checked | device pilot pending | timestamp and sequence must both match; missing evidence remains a failure |

T0 sends QUIC `RESET_STREAM` with draft-21 `DELIVERY_TIMEOUT (0x2)`, records
discarded bytes, and counts identified reset streams in `PUBLISH_DONE` receiver
accounting. A reset before the subgroup header is reported as unattributed and
invalidates exact per-track accounting. The feature is opt-in and remains off in
all S0--S5 ordering results.

## Paper comparison contract

The primary outcome is mission success over all ground-truth screw events:
correct event plus required evidence received before its deadline. Dropped,
expired, never-sampled and false-positive events remain visible in the
denominator or precision term. Secondary outcomes are event/evidence age,
preview continuity and quality, source acquisition gaps, delivered goodput,
reset/drop reason, CPU time, queue/ACK-held bytes and minimum heap.

The next experiment should move cancellation before costly transmission: use a
deadline-feasibility admission test (`remaining bytes / estimated goodput`) and
low/high or ROI representations, then retain T0 as the final overrun guard.
Compare T0 `OFF/ON` and feasibility admission under S1 using 128/256/512 kbit/s
traces, 50 ms added delay and identical deadline/memory budgets. Each condition
needs randomized repeated runs. A mechanism is useful only if it raises
mission-deadline success or bounds age/resource use without violating the
required evidence/preview floor; lower latency caused by discarding all media
is a failure.

The T0 pilot is machine-readable in
`results/t0-timeout-pilot-20260923.json`. The device was restored and verified
against baseline ELF SHA256
`0ccf545c4c5d36c9cc335bc6d21c63b2e9f6cf2eaa5035655b405e484b73b8e9`.

The generated 63-cell selector plan remains unexecuted and is stored in
`results/algorithm-plan-literature-v3.json` (SHA256
`ab6c8db2de9c0fbf5a2c3864717b88bcf924d59692aa1f46a1c8104f1c6d661f`).
T0 is represented as a paired ablation rather than multiplied into every
selector cell, which keeps ordering and cancellation effects identifiable.

## v17--v20 development cohorts

The [frozen evaluation contract](../../papers/mission_aiot_moq/EXPERIMENT_CONTRACT.md)
and [generated screening results](results/ladder-v17-screen/RESULTS.md) supersede
ad-hoc policy ranking for the new representation experiment. v17 is a randomized
five-policy/two-network screening block with pre-encoded images and identical
AI pixels. Do not merge its measurements with the earlier live camera cohort.

The first cold-start test exposed a 4.33 s initial preview gap in the adaptive
policy despite short steady-state gaps. The v20 candidate therefore starts at
the smallest representation until receiving a service sample. This is a fix
motivated by the screening data; any later result belongs to a new cohort.

v18 adds on-device conversion profiling (bounded RGB/output allocations,
independent decoder workspace), and v19 links JPEG evidence to the exact AI
source sequence and timestamp. These are opt-in and remain separate from v17.
They must be validated on device before being described as measured improvements.
None of these mechanisms reproduces a multipath paper or atomic track-switching
protocol, and none by itself establishes research novelty.


## Device validation update: v19 / v21

Exact-source evidence and on-device conversion have now run on the ESP32-S3.
The [development report](../../papers/mission_aiot_moq/DEVELOPMENT_STATUS.md)
records all counts, original failures, post-hoc accounting correction and limits.
V19 is diagnostic because synchronous conversion delayed the producer stop;
v21 independently enforces the producer cutoff. The live v21 mean acquisition
rate is approximately 10 Hz, but maximum gap is 202 ms. Conversion averages
2.424 s and only 15 of 22 delivered results have matching evidence. None of these
figures is physical screw DESR/F1 or proof of adaptive-policy superiority.


## v22/v23 scheduling and overlap follow-up

The [codec screening](results/codec-v22-screen/RESULTS.md) compares sync/worker
at low/medium quality with an independent producer cutoff. All four runs passed
accounting, fixed-input/output checks and restoration. Worker scheduling improves
pump/metadata responsiveness but does not improve medium evidence coverage.
Fixed low completes all 23 delivered-result evidence pairs in both settings.

The [selected overlap follow-up](results/codec-v23-overlap/RESULTS.md) completes
22/22 medium pairs in each of two runs, all conservatively on time. This is not
a new adaptive algorithm. Live medium validation receives 21/22 pairs but confirms
only 14 within deadline, exposing a real-workload limit. See the
[development report](../../papers/mission_aiot_moq/DEVELOPMENT_STATUS.md) for
costs, denominator limitations and the separate live low check.
