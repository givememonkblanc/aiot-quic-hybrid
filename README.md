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
│   └── components/
│       ├── presence/         # presence state machine (host-testable, no ESP deps)
│       ├── app_cfg/          # NVS-backed device configuration
│       ├── provisioning/     # SoftAP setup portal + serial console
│       ├── ota/              # esp_https_ota update + rollback confirm
│       ├── dl_qads/          # transport policy / scheduler
│       ├── transport/        # hybrid stream/datagram TX + QUIC (ngtcp2/wolfssl)
│       ├── ai_inference/     # TFLM / esp-nn wrappers
│       ├── esp_camera/, sources/, wifi_link/, common/
├── host_receiver/            # picoquic MQTT+datagram logging server
├── experiments/              # evaluation harness & datasets (research side)
├── docs/                     # PRODUCT.md, EXPERIMENTS.md
├── partitions.csv            # default (single factory app)
├── partitions_ota.csv        # OTA-capable 2-slot layout
└── sdkconfig.prod            # production hardening fragment (OTA, TLS, secure boot)
```

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
