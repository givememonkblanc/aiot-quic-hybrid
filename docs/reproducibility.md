# Reproducibility parameters

Every threshold, deadline and knob used by the transport scheduler, the edge-AI
pipeline and the evaluation, extracted from the source so results can be
reproduced. (Reviewers of QUIC-transport papers consistently require this.)

## Platform

| Item | Value |
|------|-------|
| MCU | ESP32-S3 (dual-core, 240 MHz), 8 MB PSRAM |
| Camera | OV2640, `FRAMESIZE_QQVGA` 160×120, JPEG quality 12 |
| QUIC / TLS | ngtcp2 (vendored) + wolfSSL 5.8.2 with `WOLFSSL_QUIC`; esp-http3 1.3.3 |
| Transport MQTT | coreMQTT over QUIC (streams) + QUIC DATAGRAM (RFC 9221) |
| Toolchain | ESP-IDF v5.2 |

## Transport scheduler (`dl_qads`)

Decision is a pure function of (config, packet, net metrics). Thresholds:

| Parameter | Value | Where |
|-----------|-------|-------|
| Congestion predicate | `!stream_window ∨ queue>3 ∨ RTT>80 ms ∨ loss>5%` | `dl_qads.c:network_congested` |
| Severe congestion (image) | `queue>8 ∨ RTT>140 ms ∨ loss>12%` | `dl_qads.c` stream-first |
| Hard-drop | `loss>20%` (except CRITICAL) | `dl_qads.c` |
| AoI shed | loss-tolerant ∧ `remaining_deadline ≤ RTT` ∧ congested (except CRITICAL) | `packet_past_useful_life` |
| Telemetry/AI deadline | 500 ms (`APP_DEADLINE_CLASS1_MS`) | `app_config.h` |
| Image deadline | 200 ms (`APP_DEADLINE_IMAGE_MS`) | `app_config.h` |
| Control loop period | 40–60 ms (preset) | `APP_CONTROL_PERIOD_MS` |
| Image datagram budget | 900–1300 B (preset) | `APP_IMAGE_DATAGRAM_BUDGET` |
| Stream IDs | AI=0, sensor=4, image=8 | `app_config.h` |
| Policy mode | `HYBRID_BASELINE`(0) / `RELIABILITY`(1) / `THROUGHPUT`(2) / `STREAM_FIRST`(3) | `qads_policy_mode_t` |

## Semantic (perception-aware) QoS

| Parameter | Value | Where |
|-----------|-------|-------|
| Importance = CRITICAL if | detection state change **or** max confidence ≥ 180/255 (~70.6%) | `AIOT_IMPORTANCE_HIGH_CONF` |
| Importance = ELEVATED if | any detection, confidence < 180 | `ai_result.c` |
| CRITICAL treatment | never AoI-shed; keeps reliable stream under congestion | `dl_qads.c` |

## Edge-AI pipeline

| Parameter | Value |
|-----------|-------|
| Model | Visual Wake Words person detection, MobileNet, 96×96 grayscale int8 |
| Person-score threshold | 150/255 (~59%) |
| Tensor arena | 150 KB (PSRAM) |
| JPEG decode | full QQVGA 160×120 → PSRAM buffer; model input downsampled to 96×96 |
| Live-stream cadence | 350 ms (~3 fps) |

## netem sweep (evaluation)

| Condition | netem (bidir, iface `wlp194s0` via ifb) | ≈ RTT | loss |
|-----------|------------------------------------------|-------|------|
| clean | off | ~8 ms | 0% |
| rtt100 | `delay 50ms 10ms rate 8mbit` | ~100 ms | 0% |
| rtt300 | `delay 150ms 20ms rate 8mbit` | ~300 ms | 0% |
| rtt500 | `delay 250ms 30ms rate 8mbit` | ~500 ms | 0% |
| loss2 | `delay 50ms 10ms loss 2% rate 8mbit` | ~100 ms | 2% |
| loss10 | `delay 50ms 10ms loss 10% rate 8mbit` | ~100 ms | 10% |

- **bidir** (egress + ingress via `ifb0`) so the uplink datagrams also see loss/delay.
- Modes: `stream_only`, `datagram_only`, `hybrid` (APP_TRANSPORT_MODE).
- Per condition: device reset → fresh QUIC handshake → capture window.
- **loss is emulated (netem), not a real wireless channel.**

## Measurement definitions

| Metric | Definition | Source |
|--------|------------|--------|
| Freshness (e2e) | capture timestamp → transport enqueue, per object | `tx_events.csv e2e_us` |
| RTT | QUIC smoothed RTT sample | `telemetry.csv rtt_ms` |
| Rx gap-loss % | receiver-side per-track sequence-gap loss | `receiver_dgram_summary.json` |
| Datagram loss (ACK) | ngtcp2 `lost_datagram` callback count | `telemetry.csv datagrams_lost` |
| Resource (CPU) | overall busy % and `net_tx` task CPU % | `telemetry.csv cpu_usage`, `[CPU-STATS]` |
| Shed | expired + evicted datagrams | `telemetry.csv datagram_expired/evicted` |

> Statistical reporting: results are aggregated over **N repetitions** per
> condition; report mean and dispersion (std / 95% CI). Single-rep numbers are
> not reported as findings.
