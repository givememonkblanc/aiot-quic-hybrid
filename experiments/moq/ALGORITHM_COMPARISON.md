# MoQ algorithm comparison protocol

Status: six data selectors and two bounded preview admission modes are now
implemented behind `AIOT_MOQT_POLICY_EXPERIMENT`. A six-configuration, single-run
unshaped diagnostic uses repeated fixed JPEG input and actual ESPDet inference.
See [implementation and diagnostic results](POLICY_RESULTS.md). This is distinct
from the still-unexecuted 63-cell annotated mission/network experiment below.
The newly added S5 selector passed host tests, the full ESP32 build and a bounded
one-slot device pilot. It has not yet been included in the repeated mission
matrix. Its literature provenance and the later timeout/ABR/multipath
families are defined in [the literature baseline map](LITERATURE_BASELINES.md).

현재 실행 우선순위와 단계별 진행 기준은 [연구 로드맵](RESEARCH_ROADMAP.md)을 따른다.
아래63개 mission cells는 여전히 미실행 계획이다. 로드맵의60-run 정적 제어
진단은 별도 목적이며, 두 계획을 자동으로 합쳐 실행하지 않는다.

## Research question

Under the same input sequence, resource budget, AI workload, transport and
network trace, which admission/selection policy delivers more correct important
AI events and required evidence by their deadlines while maintaining acquisition?
A smaller queue or more admitted-object receipts alone is not mission success.

## Separate the two decisions

Admission decides which unsubmitted preview to retain. Scheduling decides which
eligible track/object supplies bytes to QUIC next. Keep AI input sampling fixed
in the first comparison so a communication-policy effect is not confused with a
change in model inputs. Keep the MoQ packet-work budget and cipher fixed too.

| ID | Preview admission | Data scheduling | Role |
|---|---|---|---|
| A0 | Bounded FIFO, reject new on full | Strict AI first | Admission baseline |
| S0 | Latest pending preview | FIFO by object admission | No AI-preference baseline |
| S1 | Latest pending preview | Strict AI first, oldest JPEG | Current policy family |
| S2 | Latest pending preview | Byte-based DRR, AI:preview=1:1 | Fair-share baseline |
| S3 | Latest pending preview | Earliest absolute deadline | Deadline baseline |
| S4 | Latest pending preview | Importance, then absolute deadline | Semantic/deadline baseline |
| S5 | Latest pending preview | Draft-21 priority, descending preview group | MOQT-native ordering baseline |

Compare A0 versus S1 to isolate retention. Compare S0–S5 to isolate selection.
Do not treat all seven as an admission-only or scheduler-only experiment. S4 is a
simple heuristic comparator, **not a novel proposed framework**. Dynamic
network/memory adaptation is a subsequent candidate only if these static
baselines show a reproducible service trade-off.

## Implementation contract and remaining validation

- One JPEG data object in flight for every row. The reserved AI object slot,
  control-stream reserves, AI core/priority, resolution and model are identical.
  Apply the configured memory ceiling to pending, staging and immutable
  retransmission/ACK-held buffers; log allocated capacity as well as payload.
  Freeze the common byte budget after instrumented validation, before collecting
  comparative results. FIFO never gets an unbounded queue.
- Preview pending capacity is identical for A0 and S1. Full FIFO rejects the new
  image; latest replaces only an unsubmitted old preview. Already-submitted bytes
  cannot be overwritten. Both omissions count against all-input delivery.
- AI results use the same bounded event FIFO across policies. Do not silently
  coalesce distinct events or poll only the latest result and call that delivery.
  The diagnostic implementation uses a 32-result FIFO instead of the latest-result observation shortcut.
- Control/handshake processing is outside data-policy arbitration and retains
  protocol progress priority for every row.
- All selectors get the same maximum 1024-byte application data slice per
  decision. A slice is not a new MoQ object, nor a QUIC DATAGRAM. MoQ group/object
  boundaries and normal QUIC packetization are unchanged.
- FIFO selects the eligible object with the earliest admission (stable object-ID
  tie break), including a partially submitted object until it finishes. AI-first
  can select AI at the next slice boundary, then oldest preview.
- DRR uses a 1024-byte quantum per nonempty track, 1:1 weight initially, debiting
  **bytes actually accepted by QUIC**, not proposed bytes or completed objects.
  Empty tracks do not accumulate unlimited credit. Blocked writes do not spend
  credit; unsuccessful selection must not spin/starve the other eligible track.
  A weighted 4:1 AI ablation comes later, not mixed into the initial DRR result.
- EDF uses absolute source/event timestamp + a predeclared class-relative
  deadline, never queue-admission time + deadline. Importance-EDF sorts on
  preassigned importance before the same deadline. A result cannot gain
  importance from future labels/network outcomes. With equal relative deadlines
  and arrival-ordered input, EDF can collapse to FIFO: that is a valid result,
  not evidence to manufacture workload differences.
- Main comparison uses the same unsent-expiry rule for every selector. Transport
  cancellation is a separate ablation. No row alone gets reset/expiry advantages.
  T0 now reconciles identified timeout resets with PUBLISH_DONE and receiver
  accounting, while pre-header resets remain explicitly unattributed. It must
  pass the paired device lifecycle test before entering any result matrix.

## Workload and networks

Stage 1 replays an immutable trace (input timestamps, JPEG sizes/bytes, and
result events) with a SHA256 manifest to isolate transport selection. Existing
single-JPEG repetition is useful for microbenchmarks but has no event ground
truth and is not enough for a mission-quality claim. Replay input rate is not
physical camera acquisition rate. Keep physical 10 Hz collection verification
as a distinct concurrent/live-device check.

Stage 2 performs real local inference on the same annotated sequence and counts
all GT events, including events never sampled/inferred/received. The cat224 model
is not trained for screws; none of the current zero-detection runs validates
screw accuracy. Event identity/matching, appearance time, required evidence,
importance, and event deadline must be recorded before comparisons.

Use three initial network traces: no added impairment, a controlled rate/delay/
loss trace, and a fixed-time outage/recovery trace. Network parameters are frozen
in a separately hashed trace; no nominal RTT or bandwidth is assumed from a
profile name. Apply shaping only to the experimental device flow. Host shaping
is not weak RF, RSSI variation, MAC retransmissions or actual AP handover.
Outage cells remain disabled until reconnect and object accounting are tested.

Seven unique configurations × three network traces × three repetitions = **63
planned cells**, with randomized policy order inside each network/repetition
block and one common input/impairment realization within a block. Three runs
are a pilot, not sufficient for reliable p99 inference or paper-level power.
Use a separate tuning trace; do not choose thresholds on the reporting trace.

## Outcomes and gates

Primary: correct important-event deadline success / all relevant GT events;
required evidence must also complete if part of the mission. Report false
positives and event-level precision/recall/F1. mAP is secondary only if compatible
spatial annotations and detector output exist. Annotated-event deadlines define
mission success; arbitrary curves alone are sensitivity analysis.

Secondary: complete-preview goodput, acquisition gaps/rate, image and event
age, service update gaps, per-class omissions, AI wall/CPU latency, RAM/PSRAM,
queue bytes, slot residency, QUIC CPU and scheduler cost. Record selection,
admission rejection, coalescing, expiry, reset, connection failure and decoder
failure separately. Never report conditional delivered-only p95 as reliability.
Capture-to-receive delay requires clock-offset/uncertainty measurement. Receiver
AoI is time since the source timestamp of the freshest received update, including
intervals with no delivery; a per-object latency is not the same statistic.

Readiness gates before running comparative cells:

1. Frozen input/GT and network manifests, common memory budget and class deadlines.
2. Common policy interface plus fairness/priority/expiry/starvation regression tests.
3. Per-event queue and complete source→inference→admission→receive IDs/timestamps.
4. Clock uncertainty, reset/stream-count and reconnect validation where applicable.
5. Same-input policy replay and code/firmware/config hashes in every run.

Current state: all six selectors, bounded preview admission, the AI result FIFO,
source-relative pre-admission expiry, single-JPEG replay, and opt-in T0
post-admission stream reset are implemented. T0 has passed receiver tests and a
full ESP32 build, but no device run because the board was not enumerated.
`test_scheduler.c` checks selection, partial/failed writes and DRR byte fairness.
The diagnostic does not freeze an output event trace: local inference completion
and sampling can vary with contention. Annotated multi-image input, mission GT,
clock uncertainty, T0 device lifecycle and reconnect validation remain open.
Old DATAGRAM/FOMO runs cannot fill these cells. `algorithm_matrix.py` deliberately
continues to generate `planned_not_executed` entries for the mission matrix.

## Primary references and adaptation boundary

- [RFC 8290](https://www.rfc-editor.org/rfc/rfc8290.html), sections 3–4:
  byte-based deficit scheduling within FQ-CoDel. Our intended DRR selector is a
  track-level adaptation; it is **not** a reproduction of complete FQ-CoDel/CoDel.
- [Liu and Layland, 1973](https://www.cs.ru.nl/~hooman/DES/liu-layland.pdf):
  deadline-driven scheduling. Its real-time task assumptions do not establish
  deadline guarantees for ESP32, nonpreemptible crypto, QUIC or lossy Wi-Fi.
- [MOQT draft-21](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/21/),
  section 5.1.2: subscriber priority, publisher priority and group order define
  the native ordering baseline S5. Delivery timeout/reset remains a separate T0
  ablation and is never enabled for only one row of the ordering comparison.
- Existing MoQ wire profile remains draft-21, documented in README.md. Scheduling
  policy is application behavior, not evidence of new protocol conformance.
