> Subsequent audit correction: the original A0-retry row logged599 source IDs
> but its device final count was600. Its per-delivered-object age values remain
> descriptive, but the599 all-input denominator was incomplete. The stricter
> repeat comparison excludes that row rather than reconstructing an unknown ID.
> See [repeat audit](REPEAT_RESULTS.md). Original artifacts are retained.

# MoQ policy diagnostic, v6

Six configurations implement the admission/selection separation in
[ALGORITHM_COMPARISON.md](ALGORITHM_COMPARISON.md). This diagnostic is distinct
from the planned 54-cell mission experiment. Six valid trials completed; one telemetry-invalid attempt was retained separately.

## Common configuration

- ESP32-S3, native MOQT draft-21 project subscriber, existing pinned QUIC/TLS.
- One immutable JPEG stream slot and one reserved AI stream slot; control first.
- 1024-byte data selection limit for every policy. This is a stream write slice,
  not an application fragment or QUIC DATAGRAM. FIN only accompanies the final
  slice. Blocked streams are skipped within the bounded pump.
- Repeated 145989-byte 1024×1024 JPEG, SHA256
  `aa3c83c12c4e5acd1775acb9208499ba62c8ad9687e2413cc44dd4f579997fae`.
  Replay releases on a 100 ms grid for about 60 s, followed by drain. This does
  **not** measure physical acquisition at 10 Hz. Camera hardware remains enabled.
- Actual ESPDet cat224 inference on CPU1, priority0; this is not a screw detector.
  AI input offer cadence is common, but actual sampled/completed sequences can
  vary with contention. No frozen output event trace or object ground truth.
- Common preview pending/staging capacity 256 KiB each, immutable transmitted
  JPEG allocation, retained AI decoder buffer, and 32-result AI FIFO. FIFO-full
  rejects new results; it never silently merges different results.
- Diagnostic source-relative thresholds: JPEG2s and AI5s. These are not measured
  application requirements. All policies expire only before opening a stream;
  already submitted data stays immutable until ACK. No stream cancellation.
- AI importance2 for nonzero detections, otherwise1; preview0. In the current
  negative-detection workload, importance-EDF can reduce to AI-first behavior.
- No added network impairment, one replication each. S1 first; seeded remaining
  order S0,S3,S4,A0,S2. Wireless variation and run order are uncontrolled factors.

| ID | Preview retention | Data selector |
|---|---|---|
| A0 | One pending FIFO, reject new | AI first |
| S0 | Latest pending | Oldest admission first |
| S1 | Latest pending | AI first |
| S2 | Latest pending | Byte DRR, equal 1024-byte quantum |
| S3 | Latest pending | Earliest source-relative deadline |
| S4 | Latest pending | Importance, then earliest deadline |

DRR debits bytes accepted by QUIC, including MOQT stream headers. Failed writes
spend no credit; empty tracks reset credit and blocked tracks retain it. This
is track-level DRR, not a reproduction of FQ-CoDel. EDF is a selection heuristic,
not a deadline guarantee. The policy flags are opt-in; normal firmware defaults
remain outside this experiment.

## Measurement interpretation

`MOQ_ADMITTED` and `MOQ_RECEIPT` share source/group IDs. Device source→stream-ACK
age uses one clock and is an upper-bound proxy for receiver completion, **not**
exact source→server latency or receiver AoI. An ACK later than the threshold
does not prove the receiver itself missed it. Receiver payload hashes, admitted
IDs, received IDs and ACK IDs must agree. All actual AI completions remain in
the denominator, including queue rejection, unsent expiry and cutoff omissions.
All replay offers, including replaced/rejected previews, remain in the preview
denominator. Conditional age percentiles describe only acknowledged objects.

Replay logging and offer logging are separate. An occasional missing `[REPLAY]`
serial record is reported as `replay_log_missing`; the complete `MOQ_CAPTURE`
sequence set and trial offer count determine the denominator. The legacy
`MOQ_CAPTURE_END failed` field can count a poll with no new replay slot; it does
not establish a physical camera acquisition failure. No GT-level recall, DESR,
energy, task CPU attribution or p99 reliability claim is made here.

## Artifacts and reproduction

- `moqt_scheduler.c/.h` in `src/components/transport`: portable selector.
- `test_scheduler.c`: ASan/UBSan ordering, progress, partial-write and fairness tests.
- `policy_pilot.py`: opt-in build/test orchestration; restores original full flash
  after every device trial. It requires the S1 baseline outcome first.
- `device_trial.py --dual --replay --policy NAME --image-slots 1 --name UNIQUE`:
  validates the already-built expected firmware, inference and transport lifecycle.
- `analyze_policies.py RESULTS_DIRECTORY`: checks object/event accounting and
  calculates age/deadline curves. See ignored `results/policy-v6-{ID}/` folders.
- `results/policy-pilot-v6.json`: input hash, seed order and diagnostic scope.

Next: repeat these conditions to quantify variation, then introduce a fixed
flow-scoped degradation trace. Annotated input and required evidence must be
specified before measuring mission QoS; reconnect and cancellation need their
own lifecycle validation before outage experiments.

## Invalid attempt retained

The first `policy-v6-A0` attempt failed telemetry validation: 24 ESPDet phase
records but23 AI profile/production records. The unmatched phase's result has a
transmission record, which suggests missing telemetry rather than establishing
a decoder/model failure. The original outcome's generic failure message is
retained as provenance; the harness wording is clarified for future runs.
This attempt is excluded, not relabelled successful. `policy-v6-A0-retry` uses
the identical firmware SHA and passed complete accounting. S2 was then launched
separately because the orchestrator correctly stopped at the invalid trial.

## Measured results

Each valid trial generated24 actual AI results and received all24. JPEG values
refer to complete hash-verified objects, not packet counts. Ages below are
device source→ACK proxies; p95 is descriptive for a small sample (24 AI objects).

| Configuration | JPEG received / offered | JPEG age p95 (s) | AI age p95 (s) | AI admission→ACK p95 (s) | JPEG ACK ≤2s / offered | AI ACK ≤5s / produced |
|---|---:|---:|---:|---:|---:|---:|
| A0-retry | 40/599 | 3.115 | 3.148 | 0.071 | 1/599 | 24/24 |
| S0 | 33/600 | 2.055 | 4.541 | 1.711 | 18/600 | 23/24 |
| S1 | 33/600 | 2.063 | 3.134 | 0.096 | 16/600 | 24/24 |
| S2 | 32/600 | 2.055 | 3.174 | 0.093 | 11/600 | 24/24 |
| S3 | 33/600 | 2.041 | 4.552 | 1.593 | 14/600 | 24/24 |
| S4 | 33/600 | 2.049 | 3.110 | 0.095 | 15/600 | 24/24 |

AI wall-time medians were2.522–2.556s. Replay offer rates were9.970–9.987Hz;
this average does not establish a100ms physical acquisition deadline. Sampled
minimum free internal heap was32187–32323bytes; minimum free PSRAM was
1912916–1919104bytes. These are sampled minima, not continuous allocator low-water
marks. All valid runs had zero AI FIFO drops and no unaccounted AI results at cutoff.

FIFO selection delayed AI transmission compared with AI-first while delivering
the same number of objects. EDF also delayed AI because preview uses the earlier
class-relative deadline. DRR and importance-EDF did not show a clear advantage
over simple AI-first in this workload. A0 retained older previews: it delivered
more JPEG objects but had worse image age. Its lower replacement/copy activity
and run-to-run wireless variation are potential throughput confounds; this
single run cannot isolate those causes.

This supports keeping latest+AI-first as the next baseline, not declaring a
new algorithm superior. The diagnostic thresholds produce low all-input image
success even when every admitted object arrives; admitted-only reliability
would hide the many omitted images. There is still no mission-level success
measurement, annotated screw detection result, RF handover or degraded-network
comparison here.

Validation: firmware builds passed; portable scheduler tests passed with
ASan/UBSan; source hashes stayed unchanged across the sweep; receiver hashes,
admission IDs, receipts, inference completion IDs and normal close reconciled
for all six included runs. The first A0 telemetry-invalid run remains excluded.
Raw logs/images/firmware are in ignored result directories. Final original
firmware restoration is checked separately in `results/restore-verification.json`.

Final restoration passed: all five runtime-setting ACKs and8 AI runtime samples
on original CPU0 were observed. No NVS save was issued; experimental UDP14568
listener was closed. The board is back on its original FOMO firmware.
