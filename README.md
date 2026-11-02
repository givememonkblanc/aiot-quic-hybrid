# Media over QUIC 기반 ESP32 Visual AIoT

소스 탐색: [MoQ 코드 구조](experiments/moq/CODE_MAP.md) ·
[검증 실행 방법](experiments/moq/tests/README.md) ·
[현재 연구 상태](papers/mission_aiot_moq/DEVELOPMENT_STATUS.md).
아래 날짜별 측정은 각 실험 당시의 기록입니다.

2026-09-22 방향 변경: 영상·AI 결과 전송을 **Media over QUIC Transport(MOQT)**로 전환합니다. QUIC 자체를 교체하는 것이 아니라, 기존 자체 application framing 위주의 미디어 경로를 MOQT 세션·구독·객체 모델로 바꾸는 작업입니다.

[MoQ 전환 설계와 완료 기준](experiments/moq/README.md)을 현재 전송 계층의 기준으로 삼습니다. [MoQ 최소 전송 실험](experiments/moq/RESULTS.md)에서 실제 ESP32의 JPEG 객체30개 수신·해시 검증·정상 종료를 확인했습니다. **시험 후 운영 보드는 기존 프로토콜로 복구했습니다.** [독립 수집·AI 트랙 통합 시험](experiments/moq/CONCURRENT.md)에서는 평균9.95Hz 수집과 ESPDet 결과27건 수신을 확인했습니다. 최대 수집 간격315ms, 영상15/598장 수신으로 엄밀한10Hz 보장과 영상 QoS는 미달이며, 전체 규격 상호운용도 미완료입니다. 기존 측정값은 MoQ 성능 결과가 아닙니다. 전환 후에도 데이터 수집10Hz와 임무 성공률이 목표이며, 메모리 생존이나 폐기율만으로 성공을 판정하지 않습니다.

[후속 송신 루프·슬롯 개선](experiments/moq/PUMP_RESULTS.md)에서는 완성 영상39장과 최대 슬롯 점유3.24초를 확인했습니다. AI 지연 증가와 수집 간격 초과가 남아 있어 연구용 opt-in으로 유지합니다.

[영상 슬롯1개·2개 비교](experiments/moq/SLOT_COMPARISON.md)도 완료했습니다. 슬롯 점유 p50은1.73/2.92초, 완성 영상은34/41장으로, 다음 최신영상 기준선은1개를 사용합니다. AI 추론약2.5초와 수집 간격 초과는 미해결입니다.

아래는 전환 이전의 구현·연구 기록입니다.

# AIoT 서비스 QoS와 MCU–Edge/Cloud 협력 추론

현재 최상위 목표는 **제한된 ESP32-S3 자원에서 AI 이벤트의 제시간 전달, 영상의 신선도·연속성, 장치 안정성을 함께 높이는 것**입니다.
[QoS 고도화 로드맵](experiments/aiot_qos/ROADMAP.md)에 따라 [QUIC batching 비교](experiments/quic_batch/RESULTS.md), 서비스 계측, 고정 QoS 보호 설정, 필요한 구간의 피드백 제어 순서로 진행합니다.

**ESP32-S3가 영상에서 어디까지 처리하고 어떤 표현을 Edge/Cloud VLM에 전달할 것인가**는 이 목표를 위한 후속 연구 축으로 유지합니다.
[협력 추론 연구 계획](experiments/vlm_split/README.md)과 [문헌 지도](experiments/vlm_split/READING.md)를 함께 참고합니다.

기존 [FOMO 코어·우선순위 실험](experiments/aiot_resource_management/SWEEP_RESULTS.md)은 정적 설정 기준선으로 보존합니다.
[핸드오프/QUIC 실험](experiments/handoff/README.md)과 [Linux MP-QUIC](experiments/mobility/README.md)은 기존 측정 자산이며, 새 동적 스케줄러 개발은 현재 주 목표가 아닙니다.
[Wi-Fi OTA](experiments/ota/README.md)를 통한 기존 보드 운영은 유지합니다. 아래는 기존 구현 설명이며 새 VLM 파이프라인의 구현 완료를 뜻하지 않습니다.

# AIoT Presence Sensor — MQTT-over-QUIC Hybrid Transport

An edge-AI **presence-detection device** (존재감지장치) for MCU-class hardware
(ESP32-S3 + OV2640), built on a network-aware MQTT-over-QUIC transport that
routes each traffic class to the QUIC service its semantics require. On-device
TinyML decides `present`/`absent`; a single QUIC connection carries reliable
control/events on **streams** and loss-tolerant telemetry/snapshots on
**datagrams**, protecting the control channel under congestion.

> This repository has two faces that share one transport:
> a **product** (default) and a **research/experiment** harness. See
> **[docs/PRODUCT.md](docs/PRODUCT.md)** and
> **[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md)**.

## Highlights

- **MQTT over QUIC** — control (CONNECT/SUBSCRIBE/PUBLISH) on reliable QUIC
  streams; a normative rule of the design, since control on datagrams never
  yields a CONNACK.
- **Hybrid scheduler (`dl_qads`)** — routes payloads STREAM / DATAGRAM / DEFER /
  DROP from live RTT, queue depth, and loss, with MOQT-style per-object
  delivery timeouts (age-of-information shedding).
- **On-device TinyML** — TFLM/esp-nn inference on camera frames, debounced into
  presence state with hysteresis.
- **Product-grade lifecycle** — field provisioning (SoftAP portal + serial
  console), NVS-backed config with no secrets in the image, a session
  supervisor that reconnects with backoff, and OTA with rollback.

## Quick start (product)

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

An unprovisioned device boots into a Wi-Fi setup AP `AIoT-Presence-XXXX` →
open <http://192.168.4.1/> and enter Wi-Fi + server. Or provision over the USB
serial console (`set wifi.ssid …`, `save`, `reboot`). Full walkthrough:
**[docs/PRODUCT.md](docs/PRODUCT.md)**.

Reporting once provisioned:

| Payload | Topic / track | QUIC service |
|---|---|---|
| Presence event (enter/exit) | `aiot/presence/event` | stream (reliable, ordered) |
| Heartbeat (state, score, RSSI, heap) | telemetry track | datagram + delivery timeout |
| Snapshot JPEG on enter | image track | datagram chunks, loss-tolerant |
| Commands (OTA, …) | `aiot/<device_id>/cmd` | stream (subscribe) |

## Repository structure

```
├── src/
│   ├── main/                 # app orchestration; product & experiment entry points
│   ├── components/           # FIRST-PARTY only
│   │   ├── presence/         # presence state machine (host-testable, no ESP deps)
│   │   ├── app_cfg/          # NVS-backed device configuration
│   │   ├── provisioning/     # SoftAP setup portal + serial console
│   │   ├── ota/              # esp_https_ota update + rollback confirm
│   │   ├── dl_qads/          # transport policy / scheduler
│   │   ├── transport/        # hybrid stream/datagram TX (QUIC via third_party)
│   │   ├── ai_inference/     # TFLM / esp-nn wrappers
│   │   └── sources/, wifi_link/, common/, status_led/
│   └── third_party/          # VENDORED: wolfssl, ngtcp2, coreMQTT, esp-http3, esp_camera
├── host_receiver/            # picoquic MQTT+datagram logging server
├── experiments/              # evaluation harness & datasets (research side)
├── docs/                     # PRODUCT.md, EXPERIMENTS.md
├── partitions.csv            # default (single factory app)
├── partitions_ota.csv        # OTA-capable 2-slot layout
└── sdkconfig.prod            # production hardening fragment (OTA, TLS, secure boot)
```

## What's ours vs vendored

This repo is **~96% third-party by size** — `wolfssl` alone is ~50 MB — because a
TLS/QUIC/TinyML substrate is large and off-the-shelf. Size is the wrong ruler.
The contribution is a compact layer of **integration, product, and measurement**
on that substrate, not the substrate itself.

**Ours** — ~11k lines of first-party code:

| Area | LOC | What it is |
|---|---|---|
| `transport/` + `dl_qads/` | ~2.9k | hybrid stream/datagram routing, freshness-first (AoI) shedding, the scheduler — the design's **novel core** |
| `main/` | ~2.0k | product + experiment orchestration |
| `ai_inference/` | ~1.1k | TFLM integration (classification / FOMO / motion paths) |
| presence · provisioning · ota · app_cfg · status_led | ~1.3k | product plumbing |
| `backend/` + `experiments/` | ~2.9k | dashboards, netem sweep, analyzers |

Much of `transport/` is glue that wires ngtcp2 into ESP-IDF; the genuinely novel
code is small (the routing decision + the `dl_qads` scheduler + the measurement
methodology). That is normal for applied systems work.

**Vendored** — `src/third_party/` (upstream, unmodified except one patch):
wolfSSL (local `WOLFSSL_QUIC` patch), ngtcp2, coreMQTT, esp-http3, esp32-camera;
plus managed deps (TFLM, esp-nn, esp_jpeg) and picoquic on the host. Provenance
and the wolfSSL patch are documented in
**[src/third_party/README.md](src/third_party/README.md)**.

**So the contribution is architecture + evidence, not a new stack.** The novel
results — sender freshness staying ~constant under RTT/loss, and datagrams
costing *more* CPU than streams on an MCU — come from the integration and the
measurement, not from reimplementing QUIC.

## Configuration

Compile-time defaults live in `src/main/app_config.h` (Wi-Fi/server default to
`REPLACE_ME`; the live values come from NVS provisioning). `APP_PRODUCT_MODE`
selects product (1, default) vs experiment (0) builds.

## Security

Production checklist (details in [docs/PRODUCT.md](docs/PRODUCT.md#security)):
QUIC server-cert pinning (drop `src/components/transport/server_ca.pem`), TLS
for OTA, and — after validating on a sacrificial board — Secure Boot V2 + Flash
Encryption from `sdkconfig.prod`. Real keys/certs are gitignored.

## Research & evaluation

The transport is the subject of a standardization paper; the evaluation
harness, datasets, and the `APP_PRODUCT_MODE 0` build are documented in
**[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md)**.

## License

Integrates coreMQTT (MIT), picoquic/picotls (MIT), wolfSSL (GPLv2), ngtcp2
(MIT), and TensorFlow Lite Micro (Apache 2.0). See per-component licenses.
