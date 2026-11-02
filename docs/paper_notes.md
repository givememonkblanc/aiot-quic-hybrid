> 2026-09-21 방향 전환: 현재 연구 질문과 계획은 [MCU–Edge/Cloud VLM 협력 추론](../experiments/vlm_split/README.md)을 따른다. 아래 scheduler/transport 기여 주장은 과거 초안이며 현재 논문 기여로 채택하지 않는다.

> 2026-09-20 방법론 점검: 아래 수치는 과거 실험 메모이며 현재 카메라/이동성 펌웨어나 외부 논문의 재현 결과가 아니다. OWDV는 지연 변동량이며 절대 receiver AoI가 아니다. 신뢰성 STREAM과 deadline 폐기 DATAGRAM의 차이도 transport만의 효과로 해석하지 않는다. 현재 문헌·KPI·기준선 검증은 [MQTT/QUIC 재현 조사](../experiments/mqtt_quic_papers/README.md)를 따른다.

# Paper notes — contribution framing, related work, writing checklist

Consolidated from peer-review feedback on a sibling MP-QUIC paper (the same
reviewers' expectations apply here). Fill exact citations before submission.

## Contribution / novelty (state this explicitly — reviewers ask "what's novel")

We do **not** propose a new QUIC or TLS stack. We integrate off-the-shelf QUIC
(ngtcp2 + wolfSSL) on an MCU and contribute:

1. **A hybrid MQTT-over-QUIC transport profile** for heterogeneous MCU-AIoT
   traffic — control on reliable streams (a normative rule: control on datagrams
   never yields a CONNACK); telemetry / AI-result / image on datagrams with
   MOQT-style per-object delivery timeouts and AoI-aware shedding.
2. **A perception-aware (cross-layer) scheduler** — the AI detection's semantic
   salience (state change / high confidence) drives the transport QoS: CRITICAL
   is kept reliable and never AoI-shed; routine telemetry degrades first
   (`dl_qads`; isolated by a semantic ON/OFF decision matrix).
3. **An empirical MCU-class evaluation** whose results differ from prior work:
   on an MCU, **datagrams cost more CPU than streams** (per-object framing + ACK
   loss-detection callbacks), inverting the DNS-over-QUIC (Pi-Zero) finding; and
   a measured **"why QUIC"** result — under loss, TCP-MQTT's AoI collapses to
   ~1.5–3.7 s (head-of-line blocking) while QUIC-hybrid holds ~100 ms (~35×).

Frame it as **architecture + evidence**, not "we built a QUIC stack".

## Related work to cite (by topic)

- **QUIC & datagrams**: RFC 9000 (QUIC), RFC 9221 (unreliable datagram
  extension), RFC 9000 §9 (connection migration — relevant to the SAR/mobility
  direction), RFC 9114 (HTTP/3).
- **Resource-conservation baseline we build on**: Saif & Matrawy, "A Datagram
  Extension to DNS-over-QUIC" (the Pi-Zero resource study we extend to MCU).
- **MOQT**: IETF Media-over-QUIC Transport draft (the per-object delivery-timeout
  / priority model the hybrid profile borrows).
- **MQTT**: OASIS MQTT v5.0; MQTT-SN for constrained devices.
- **Age-of-Information**: Kaul et al. (AoI foundational) + AoI-aware scheduling
  literature — frame the freshness/shedding contribution against this.
- **MP-QUIC schedulers** (the sibling paper's space): survey the published
  MP-QUIC scheduler proposals to position the semantic scheduler.
- **QUIC on embedded / MCU**: ngtcp2, esp-http3, and any ESP32/embedded-QUIC
  reports.
- **Cross-layer / semantic transport & QoS-aware scheduling**: position the
  perception-aware scheduler here.

## Writing / methodology checklist (from the reviews)

**Rigor**
- [ ] Report **n, mean ± std / 95% CI**, and significance — never single-rep
  numbers (sweep is n=5; see `out/paper_sweep_multirep.txt`).
- [ ] Full **parameter table** for reproducibility (`docs/reproducibility.md`):
  every scheduler threshold, deadline, netem value, model input.
- [ ] Name the **QUIC implementation**, its version, and where the scheduler
  lives; state what was modified.

**Baselines (isolate the contribution)**
- [ ] **TCP-MQTT** baseline (`docs/tcp_baseline_design.md`) — answers "why QUIC".
- [ ] **Semantic ON/OFF** decision matrix (`out/semantic_matrix.txt`) — isolates
  the perception-aware scheduler's benefit.

**Honest framing (avoid the overclaim traps)**
- [ ] No "eliminates / virtually eliminates" — state measured ranges.
- [ ] **Define metrics precisely**: "freshness" here = *sender-side* age
  (capture→enqueue), which is network-independent by construction; the network
  result is *receiver-side* **AoI** (OWDV p95). Frame claims on AoI. Note OWDV is
  a **jitter-based AoI proxy** (avoids device↔receiver clock sync).
- [ ] The scheduler is **reactive** (responds to measured RTT/queue/loss), not
  proactive/predictive — say so.
- [ ] Loss is **netem-emulated**, not a real wireless channel — state it.
- [ ] Testbed TLS for the TCP baseline was encryption-only (no broker auth);
  crypto cost identical, so performance-fair — note it.

**Explain counterintuitive results** (reviewers ask)
- [ ] Datagram costs *more* CPU than streams on MCU (framing + ACK callbacks).
- [ ] Stream RTT is *higher* than datagram (stream ACK/flow-control path).
- [ ] TCP shows *0 % loss* under netem loss but catastrophic staleness
  (retransmit + HoL) — reliability bought at the cost of freshness.

**Presentation**
- [ ] Standardize terminology — use **"Wi-Fi"** everywhere (not wifi/WiFi/Wifi).
- [ ] **Number all equations**.
- [ ] Ensure figure text/labels are legible; regenerate low-res figures.
