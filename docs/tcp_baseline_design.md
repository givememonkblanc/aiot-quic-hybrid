# TCP-MQTT baseline — design

**Why.** Reviewers of QUIC-transport papers (all three, incl. R1/R2/R3 #4)
demand a **non-QUIC baseline**: comparing our own stream/datagram/hybrid modes
does not establish *why QUIC at all*. The fair reference is **standard
MQTT-over-TCP (+TLS)** — same MQTT semantics, same workload, same network
conditions, only the transport differs. R2 explicitly expects the
head-of-line-blocking argument, so the baseline must make it measurable.

## Fairness principles (pre-empt the "unfair comparison" critique)

Everything is held equal except the transport:

| Held equal | Differs |
|------------|---------|
| Device (ESP32-S3), camera, inference, payloads, object rates | Transport: **MQTT-over-TCP+TLS** vs **MQTT-over-QUIC (hybrid)** |
| MQTT message types (control, telemetry, AI result, image chunks) | TCP = one ordered reliable byte stream; QUIC = streams + datagrams |
| TLS 1.3-class security (esp-tls/mbedTLS on TCP; built-in on QUIC) | — |
| netem sweep (same RTT/loss conditions, same iface) | UDP (QUIC) vs TCP port — netem shapes the iface either way |
| Freshness/delivery/resource definitions | — |

> The baseline carries **all** payloads as MQTT `PUBLISH` over **one** TCP+TLS
> connection — no datagrams, no independent streams. That is *standard MQTT*, and
> the fact that it cannot shed or bypass reliability is exactly what we measure,
> not an unfairness.

## Architecture — the swap point is clean

`coreMQTT` is transport-agnostic: `app_main` wires a `TransportInterface_t`
(`xTransportInterface.send/recv = mqtt_quic_transport_send/recv`) into
`MQTT_Init`. The baseline swaps only that:

```
                          ┌─ APP_TRANSPORT_BACKEND == QUIC ─ mqtt_quic_transport_* (ngtcp2/wolfSSL, UDP)
 coreMQTT TransportInterface┤
                          └─ APP_TRANSPORT_BACKEND == TCP  ─ mqtt_tcp_transport_*  (esp-tls/mbedTLS, TCP)
```

- New `mqtt_tcp_transport.c`: `send`/`recv` over an esp-tls (mbedTLS) TCP socket
  — a ~200–300 LOC transport implementing the same `TransportInterface_t`.
- A compile-time `APP_TRANSPORT_BACKEND` flag (like `APP_TRANSPORT_MODE`) selects it.
- In TCP mode, the objects that the QUIC path sends on datagrams/aux-streams
  (telemetry, AI results, image chunks) are instead sent as **MQTT PUBLISH**
  on their topics over the single TCP connection.
- Every object keeps the `aiot_object_header_t` (`gen_ts_ms`, `object_seq`,
  `track_id`) so the subscriber measures freshness identically.

## What it exposes (and the expected result)

| Effect | TCP-MQTT | QUIC hybrid | Why it matters |
|--------|----------|-------------|----------------|
| **Head-of-line blocking** | one ordered TCP stream: a lost/late telemetry or image segment stalls **all** later bytes, incl. control/AI | independent streams; datagrams bypass reliability | under loss, TCP freshness inflates while hybrid stays ~56–59 ms — the headline "why QUIC" |
| **Handshake** | TCP + TLS = multiple RTTs | QUIC 1-RTT (0-RTT resumption) | connect/reconnect latency |
| **Connection migration** | IP change (cellular handover) → full reconnect | QUIC migrates (RFC 9000 §9) | the SAR/mobility argument |
| **Age-of-Information shedding** | impossible (all reliable/ordered) | old datagrams shed to keep fresh | freshness under sustained loss |
| **Resource** | TCP+TLS buffers/state | ngtcp2+wolfSSL | MCU memory/CPU comparison |

Expected finding: **under loss the TCP baseline's freshness and delivery degrade
(HoL + retransmission) while QUIC-hybrid holds freshness**, isolating QUIC's
benefit. (R2: "QUIC intrinsically avoids HoL" — the baseline demonstrates it.)

## Host side

- **Broker**: mosquitto (TCP+TLS) on the host, replacing picoquic for TCP runs.
- **Logging subscriber**: subscribes to all tracks, stamps receive time,
  computes freshness = `recv_ms − gen_ts_ms` per object, and delivery ratio from
  `object_seq` gaps — the *same* metrics the QUIC receiver logs, so results are
  directly comparable.
- Reuse `parse_*`/`analyze_sweep.py` by emitting the same CSV schema.

## Sweep integration

- Add a `backend` axis to the sweep: `{quic-hybrid, quic-stream, quic-datagram, tcp}`.
- Same netem conditions (`clean/rtt*/loss*`), same REPS, same 45 s window.
- netem shapes the iface, so TCP (port 8883) and QUIC (UDP 14567) see identical
  RTT/loss. Point the device at mosquitto (TCP) vs picoquic (QUIC) per backend.

## Implementation plan (effort-ordered)

1. ✅ *(compile-verified)* `mqtt_tcp_transport.{c,h}` — esp-tls send/recv +
   connect/disconnect implementing coreMQTT's `TransportInterface`, with its own
   `struct NetworkContext`. Wired into the transport component (SRCS + esp-tls
   REQUIRES) and builds cleanly on esp32s3 (mbedTLS backend). Runtime
   non-blocking/WANT_READ semantics still need on-target testing (needs a broker
   + the step-2 data-plane integration).
2. `APP_TRANSPORT_BACKEND` flag; in `app_main`, include the TCP header instead of
   the QUIC one, and — the real work — **route the data plane through
   `MQTT_Publish`** instead of `send_hybrid`/datagrams (telemetry, AI result,
   image chunks as PUBLISH on their topics). This is more than a transport swap
   because the QUIC data plane bypasses coreMQTT.
3. Host: mosquitto config (TLS) + logging subscriber emitting the QUIC CSV schema. *(host, ~½ day)*
4. Sweep harness `backend` axis + point device to the right receiver. *(script)*
5. `analyze_sweep.py`: add backend to the comparison; produce TCP-vs-QUIC table.

## Fairness caveats to state in the paper

- Same TLS (esp-tls/mbedTLS vs QUIC's TLS 1.3) — not TCP-plaintext vs QUIC-TLS.
- Same payloads, rates, deadlines, netem, device.
- MQTT QoS0 for best-effort parity — but **TCP ordering still causes HoL
  regardless of QoS**, which is the point.
- Not a comparison against *other* QUIC-MQTT stacks (out of scope; cite them in
  related work instead).

## Follow-up (mobility dimension)

A second experiment — drop the link mid-run (or change the NAT/IP) — measures
**TCP full-reconnect vs QUIC connection migration** recovery time. This feeds
the SAR/drone mobility story (cellular handover) and answers R1/R2's connection-
migration comments directly.
