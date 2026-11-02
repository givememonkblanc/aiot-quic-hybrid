# Research & Experiments

현재 주 연구는 [MCU–Edge/Cloud VLM 협력 추론](../experiments/vlm_split/README.md)이다.
[FOMO 정적 설정 비교](../experiments/aiot_resource_management/SWEEP_RESULTS.md)는 기준선으로 보존하며, 아래 핸드오프·hybrid transport 내용은 기존 실험 설명이다.

The `experiments/` tree and the `APP_PRODUCT_MODE 0` firmware build are the
research side of this repository: the evaluation harness behind the
MQTT-over-QUIC hybrid-transport standardization work (KICS-level paper,
MOQT / OASIS mqtt-next / oneM2M gap analysis). They are intentionally kept
separate from the product and are **not** built into a product image.

## How the two sides are separated

- **Product** (`APP_PRODUCT_MODE 1`, default): presence device. No experiment
  scaffolding on the hot path. See `docs/PRODUCT.md`.
- **Experiment** (`APP_PRODUCT_MODE 0`): keeps the transport-mode matrix
  (`APP_TRANSPORT_MODE` = STREAM_ONLY / DATAGRAM_ONLY / HYBRID), the phased
  scenario timeline (`APP_SCENARIO_MODE_ENABLE`), and the stress burst mode
  (`APP_STRESS_MODE_ENABLE`) that drive the paper's measurements.

Both builds share the same transport, dl_qads scheduler, and QUIC/MQTT
bring-up (`app_quic_mqtt_bringup()`), so the product runs the exact transport
the paper characterizes.

## Contents of `experiments/`

- `run_v2_rep.sh`, `run_v2_scenario_matrix.sh`, `netem_stress.sh` — unattended
  capture: server + serial capture + netem shaping + offline parsers per rep.
- `telemetry_analyzer/` — firmware-CSV and receiver-datagram parsers, per-phase
  scenario analysis, and v1↔v2 comparison tables.
- `runs_v2/` — collected captures (normal + stress + scenario), one dir per rep.
- `COMPARISON_PLAN.md` — the evaluation protocol and known caveats.

See `experiments/telemetry_analyzer/README.md` for parsing/plotting.
