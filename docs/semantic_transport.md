# Semantic cross-layer transport (perception-aware QoS)

A cross-layer hint from the **perception layer** (on-device AI) to the
**transport scheduler** (`dl_qads`): the AI's confidence in a detection becomes
a QoS input, so delivery guarantees follow the *meaning* of a payload, not just
its type. This extends the design from "hybrid transport" to
**perception-aware hybrid transport** — the transport protects what the model
says matters.

## Idea

A high-confidence detection, or a detection **state change** (someone entered /
left), is a salient **event** that must arrive reliably and fresh. Steady-state
telemetry is **routine** and may be degraded or shed first under pressure.
Standard QUIC scheduling uses transport state only; here the scheduler also
reads a semantic importance derived from the inference result.

## Importance (`aiot_importance_from_result(cur, prev)`)

| Level | Condition |
|-------|-----------|
| `CRITICAL` | detection-state transition (enter/exit) **or** max confidence ≥ `AIOT_IMPORTANCE_HIGH_CONF` (180/255 ≈ 70%) |
| `ELEVATED` | any detection present (moderate confidence) |
| `ROUTINE`  | no detection |

`source_manager` owns inference (single-owner, see the pipeline), so it computes
importance each frame by comparing the new result against the previous one
(`last_result`) **before** it is overwritten, then tags every `data_packet`
(`data_packet_t.importance`). Decode hiccups (invalid JPEG) are forced to
`ROUTINE` to avoid spurious "events".

## Scheduler rules (`dl_qads_decide`)

- **CRITICAL is never AoI-shed** — the `packet_past_useful_life` and `loss > 20%`
  drops are skipped for it, so a salient event is not dropped to keep newer
  routine objects fresh.
- **Salient (`ELEVATED`+) AI results keep the reliable stream even under
  congestion** — only `ROUTINE` telemetry degrades to datagrams. The perception
  layer tells the transport this result is worth protecting.
- `importance` is echoed in `schedule_decision_t` for logging/observability.

The decision is now a function of **network state × detection salience**, which
is the novel core: a compact, principled scheduler rather than a fixed
type→channel table.

## Why this is a contribution

Coupling an AI result's semantic salience into transport QoS **on an MCU** —
"the detection's importance drives its delivery guarantee" — is specific to
AIoT and is not what off-the-shelf QUIC does. It is original work in the layer
that is genuinely ours (the scheduler), not in the vendored QUIC/TLS substrate.

## Evaluation (planned)

Measure under `netem` congestion (extend the sweep): with detections injected,
compare **CRITICAL vs ROUTINE** on delivery ratio and freshness under loss.
Expected result — CRITICAL delivery and freshness stay high while ROUTINE sheds
— demonstrates semantic protection and gives a second novel empirical result
alongside "freshness decoupled from network".

## Files

- `common/` — `data_packet.h` (importance enum + field), `ai_result.{h,c}` (derivation)
- `sources/` — `source_manager.{h,c}` (compute per frame + tag packets)
- `dl_qads/` — `dl_qads.{h,c}` (semantic decisions + echo)
