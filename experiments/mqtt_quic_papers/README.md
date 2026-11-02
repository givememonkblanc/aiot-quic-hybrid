# MQTT over QUIC 문헌 조사와 재현 기준선

조사일 2026-09-20. 원문·저자 공개 코드 기준으로 확인했다. **논문에서 정의한 전송 의미와 실험 조건부터 맞춘 뒤, ESP32 메모리 압박·이동성을 별도 확장 실험으로 평가한다.** 아직 논문 전체나 ESP32에서 논문의 수치를 재현한 것은 아니다.

확장 조사: [LITERATURE_SURVEY.md](LITERATURE_SURVEY.md). 직접 평가 논문, H3 비교 연구, 2026 투고 원고, QUIC 실측이 없는 이동성 모델을 구분했다. Saif tuning 논문의 정식 서지는 IEEE IoT Journal 11(3), 2024이며 아래의 2022/2023 표기는 검토한 공개 원고의 날짜다. 새로 확인한 스트림 매핑 원고와 출처 기록은 `survey_sources_20260920.json`에 있다.

저자 코드 실행 단계: [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md). Paho/VolantMQ의 절대경로 의존성을 저자 QUIC 포크와 Go 1.14.15로 복원해 실제 QoS0/1/2 송수신에 성공했다. 스트림 매핑 원고의 실제 Rust 구현도 빌드·실행했다. 아래의 초기 “원본 build 실패” 기록은 당시 상태이며, 현재 진행 상태와 변경 내역은 위 문서를 따른다. 전체 논문 재현·ESP32 이식은 아직 완료하지 않았다.

## 핵심 논문

| 논문 / 원문 | 확인한 구현과 KPI | 이번 연구에서의 역할 |
|---|---|---|
| Kumar & Dezfouli, **Implementation and Analysis of QUIC for MQTT**, Computer Networks 150, 2019, DOI 10.1016/j.comnet.2018.12.012. [저자 PDF](https://www.cse.scu.edu/~bdezfouli/publication/QUIC-MQTT-COMNET2019.pdf) | MQTT client/server agent 통합. 연결 수립 패킷 수, CPU·메모리, 메시지 지연, migration 중 throughput 저하. 원문은 Google QUIC/Crypto 계열 | 이동성·자원 KPI의 출발점. 현대 ngtcp2/TLS 1.3으로 바꾸면 원본 바이너리 재현과 구분 |
| Fernández et al., **Even Lower Latency in IIoT: Evaluation of QUIC in Industrial IoT Scenarios**, Sensors 21, 5737, 2021. [원문](https://pmc.ncbi.nlm.nih.gov/articles/PMC8434189/) | quic-go 0.15.1/draft-27, Paho 1.2.0, VolantMQ 0.4.0-rc6. ns-3/emulation 기반 지연·손실 조건 비교 | 저자 공개 MQTT–QUIC 연결 코드를 확인할 직접 기준선 |
| Jeddou et al., **Delay and Energy Consumption of MQTT over QUIC: An Empirical Characterization Using Commercial-Off-The-Shelf Devices**, Sensors 22, 3694, 2022. [원문](https://pmc.ncbi.nlm.nih.gov/articles/PMC9145551/) | Raspberry Pi 기반 TCP+TLS 대 QUIC, QoS별 지연 분포·RSD, 외부 계측 에너지/평균 전력. [저자 실험 코드](https://github.com/tlmat-unican/MQTT-QUIC-for-RaspberryPi) | 재현 우선순위 1: 부하·QoS·네트워크 조건을 명시한 공개 실험 |
| Saif & Matrawy, **A Pure HTTP/3 Alternative to MQTT-over-QUIC in Resource-Constrained IoT**, 2021. [원문](https://arxiv.org/abs/2106.12684) | H3 pub/sub와 MQTT-over-QUIC의 성능·네트워크·device overhead 비교 | 다른 애플리케이션 프로토콜 비교군. MQTT 전송 계층만의 효과와 분리 |
| Saif & Matrawy, **An Experimental Investigation of Tuning QUIC-Based Publish-Subscribe Architectures in IoT**, arXiv 2022, 검토한 원문은 2023-06-28 수정본. [원문](https://arxiv.org/abs/2208.11178) | RFC9000, Pi Zero, QoS0/1, 인증·MAX_STREAM·timer 조정. Table VII의 8개 KPI: 실행 시간, 첫 data frame 시간, CPU, 메모리, 전력, 전송 바이트·패킷 수, binary footprint | tuning 실험과 KPI 정의의 기준. Go pprof 누적 allocation을 ESP32 peak heap과 같은 수치로 비교하지 않음 |

2019 논문의 개선율을 ESP32 목표치로 복사하지 않는다. 현재 스택·인증·장치·워크로드가 다르다. 2022 에너지 논문의 낮은 부하 지연 평가와 고해상도 영상 큐 포화도 같은 실험이 아니다. 문헌의 결과 방향을 사전에 정답으로 두지 않는다.

## 원본 코드 확보와 그대로 재현할 때의 제약

`source_manifest.json`에 repository URL/commit과 원문 파일 SHA-256을 기록했다. 다운로드 원문과 원본 clone은 `references/`, `upstream/`에 있으며 Git 추적에서 제외한다. 원본 라이선스는 각 저장소에 그대로 있다.

- Paho: [pgOrtiz90/paho.mqtt.golang](https://github.com/pgOrtiz90/paho.mqtt.golang).
- Broker: [fatimafp95/volantmq_2](https://github.com/fatimafp95/volantmq_2). tuning 원문의 링크 표기가 가리킨 별도 `volantmq`도 함께 확인했다.
- 실험: [tlmat-unican/MQTT-QUIC-for-RaspberryPi](https://github.com/tlmat-unican/MQTT-QUIC-for-RaspberryPi).
- Paho의 원본 build check는 저자 PC `/home/pgarrido/.../quic-go` 절대경로 `replace` 의존성이 없어 실패했다. broker도 `/home/administrator/...` 의존성이 있다. 오류는 `results/original-paho-build.log`에 보존했다.
- 옛 dependency fork와 Go toolchain, broker plugin 환경을 고정하는 복원 작업이 남아 있다. 최신 QUIC로 무조건 교체한 다음 “원문 그대로 구현”이라고 부르지 않는다. 논문 하드웨어인 Pi/외부 전력계도 이번 ESP32와 다르다.

## 재현 파라미터

기계가 읽을 수 있는 설정은 `profiles.json`이다. 원문에서 확인하지 못한 repetition/loss grid는 `null`로 남겼다.

- Fernández 2021 Table 1은 **RTT** 25/100/600ms와 20/10/1.5Mbps, 손실 0/1/2/3/5/10%다.
- Jeddou 2022 Tables 2–3의 지연 실험은 100B, 1초 간격, 1000개, QoS0/1/2다. 본문이 명시한 **one-way delay** 25/100/600ms를 2021의 RTT와 혼동하지 않는다. 에너지 실험의 50/100/500B, 1/10/20ms 간격은 별도 프로필이다.
- Saif 수정본의 load scaling은 32B, 100ms 간격, 1/10/25/50/100개, 50회 반복이다. NB-IoT 모델은 uplink 159kbps / downlink 127kbps / RTT 2초다.

현재 host smoke는 이 프로필을 실행했다고 주장하지 않는다. host NIC에 `tc`를 걸지 않았고, ESP32 스트리밍·Wi-Fi 설정·OTA는 이 조사에서 바꾸지 않았다.

## 구현한 공통 비교 도구

`benchmark.py`는 다음 두 전송을 **동일 Mosquitto**로 연결한다.

1. MQTT 3.1.1 → TLS 1.3/TCP proxy → Mosquitto.
2. MQTT 3.1.1 → QUIC reliable bidirectional STREAM proxy → 같은 Mosquitto.

구독자도 같은 전송을 사용한다. 양쪽 모두 broker 앞에 동일한 loopback bridge가 있어 QUIC 쪽에만 추가 bridge가 생기지 않는다. MQTT CONNECT/CONNACK, SUBSCRIBE/SUBACK, PUBLISH, QoS1 PUBACK을 실제 교환한다. 한 MQTT 연결은 한 ordered STREAM에 대응하며 QoS1 in-flight PUBLISH는 하나다. TLS 인증서 검증을 두 전송 모두 켰고 resumption/0-RTT는 시험하지 않았다. 로컬 broker는 loopback에만 바인딩하고 시험 종료 시 정리한다.

이 도구는 **현대 스택으로 만든 의미상 기준선**이다. 저자 Paho/VolantMQ 실행, 원문 COTS 테스트베드, ESP32 측정을 대체하지 않는다. Python aioquic과 OpenSSL TCP의 CPU 구현 차이도 포함된다.

KPI는 publisher와 subscriber가 공유하는 monotonic clock으로 측정한다. CONNECT API 시작→CONNACK, API 시작→첫 PUBLISH, publisher timestamp→subscriber 수신의 mean/p5/p25/p50/p75/p95/p99, population/sample RSD, PUBACK 시간, unique delivery ratio, payload goodput을 저장한다. 논문의 packet capture 기준 Initial→data-frame 시간과 API 호출 기준 시간을 같은 이름으로 부르지 않는다. 전력·wire bytes·ESP32 RAM·handoff KPI는 측정하지 않았으므로 `null`이다.

### 논문 KPI 수집 v2

실행 결과: [KPI_COMPARISON.md](KPI_COMPARISON.md). timing은 50회 × 두 transport × QoS0/1의 200회, 2000/2000 unique 수신을 확인했다. 별도 allocation은 그룹당 3회, 120/120 수신이다. 계산 단위 테스트 6개와 합성 pcap을 실제 tshark로 읽는 parser 검증을 통과했다. 합성 pcap 검증은 wire 실측 데이터가 아니다. 실행 소스 사본의 SHA-256과 각 raw 메시지 수를 재검증했다.

`paper_metrics.py`에 지표 정의와 미측정 사유를 두었다. 각 실행은 workload, 측정 pass, CPU 범위와 소스 SHA-256을 기록한다. 최신 실행에는 소스 사본도 보존한다.

- CPU: `getrusage(RUSAGE_SELF)`의 user+system 누적 시간 차이. publisher/subscriber/proxy가 같은 프로세스인 호스트 범위이며 Mosquitto는 제외한다. Go pprof의 클라이언트 단독 수치와 직접 비교하지 않는다.
- 실행 시간: publisher 연결 API 시작→마지막 unique 메시지 수신 및 QoS1 PUBACK 완료. subscriber 준비와 연결 종료는 제외한다. 별도 `execution_s`는 subscriber 준비도 포함하는 기존 필드다.
- 메모리: `--allocation-profile` 실행에서 memray로 Python/native allocator 요청 바이트를 각각 누적한다. 두 계층은 중복되므로 합산하지 않는다. peak RAM이나 ESP32 PSRAM이 아니며 Go pprof와 동일 구현 범위도 아니다. 계측 부담 때문에 timing pass와 분리한다.
- `pcap_metrics.py`: 실제 pcap의 flow/window filter에 대해 `frame.len`과 패킷 수를 집계한다. 복호화한 trace에서 확인한 Initial/첫 PUBLISH frame 번호를 주면 packet 기준 TtFDF를 계산한다. 둘 중 하나만 주거나 프레임이 없으면 실패한다. 필터는 한 endpoint pair의 양방향을 포함해야 하며, 여러 인터페이스의 중복 캡처를 넣지 않는다.
- `energy_metrics.py`: 외부 전력계 CSV(`timestamp_s,voltage_v,current_a`)의 V×I를 시간 적분해 J와 평균 W를 계산한다. CSV는 측정 구간 전체와 정확히 일치해야 한다. 비정상 값과 역전/중복 시각은 거부한다. 외부 계측은 아직 수행하지 않았다.
- schema 2부터 마지막 PUBLISH 뒤 불필요한 한 주기 sleep을 제거했다. 예전 `loopback-smoke-03`과 실행 시간/goodput의 종료 경계가 다르므로 직접 합산하지 않는다. 첫 실행 warm-up은 별도로 실행하며 50회 결과에는 포함하지 않는다.

실행 예시(각 output은 새 디렉터리여야 한다):

```bash
experiments/mqtt_quic_papers/.venv/bin/pip install -r experiments/mqtt_quic_papers/requirements-profiling.lock
experiments/mqtt_quic_papers/.venv/bin/python experiments/mqtt_quic_papers/benchmark.py \
  --count 10 --payload 32 --interval-ms 100 --repetitions 50 --output /tmp/paper-timing
experiments/mqtt_quic_papers/.venv/bin/python experiments/mqtt_quic_papers/benchmark.py \
  --count 10 --payload 32 --interval-ms 100 --repetitions 3 --allocation-profile --output /tmp/paper-allocation
experiments/mqtt_quic_papers/.venv/bin/python experiments/mqtt_quic_papers/summarize.py /tmp/paper-timing
experiments/mqtt_quic_papers/.venv/bin/python experiments/mqtt_quic_papers/report.py \
  --timing /tmp/paper-timing --allocation /tmp/paper-allocation --output /tmp/paper-comparison.md
```

50회 반복·32B·100ms·10개는 Saif의 load grid 중 한 점이다. NB-IoT의 RTT/대역폭, Pi Zero, H3, 원본 Go 구현은 포함하지 않아 **논문 전체 재현이 아니다**. allocation 3회는 도구 검증용이다. Jeddou의 1000개/1초/QoS2 grid도 실행했다고 주장하지 않는다.

실행:

```bash
python3 -m venv experiments/mqtt_quic_papers/.venv
experiments/mqtt_quic_papers/.venv/bin/pip install -r experiments/mqtt_quic_papers/requirements.lock
experiments/mqtt_quic_papers/.venv/bin/python experiments/mqtt_quic_papers/benchmark.py \
  --count 100 --payload 100 --interval-ms 10 --repetitions 3 \
  --output experiments/mqtt_quic_papers/results/UNIQUE_RUN
```

## ESP32와 비교하기 전 반드시 맞출 조건

현재 `demoserver.c:picoquic_mqtt_handle_packet()`는 제한된 MQTT packet 수신기다. PUBLISH payload를 기록하지만 완전한 broker fan-out/QoS1 PUBACK 처리를 제공하지 않는다. 지금 영상은 MQTT PUBLISH가 아니라 별도 QUIC DATAGRAM 조각이다. 따라서 현재 카메라 결과를 논문의 subscriber delivery KPI와 직접 대조할 수 없다.

MQTT QoS0은 MQTT 계층에서 ACK를 생략하는 것이며, 손실 가능한 DATAGRAM 전송과 동의어가 아니다. MQTT 3.1.1은 ordered/lossless underlying transport를 요구한다. 신뢰성 조건은 [OASIS MQTT 3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)에 맞춘다. QUIC의 단일 STREAM 내부에서도 순서 의존 지연은 남는다.

프로젝트 baseline은 **MQTT/TCP+TLS → MQTT/QUIC 단일 STREAM → 같은 STREAM+migration → 현재 hybrid DATAGRAM → hybrid+AMR admission** 순서로 분리한다. 마지막 두 항목은 신뢰성·deadline 폐기 의미가 달라지는 확장군이며 “QUIC만 바꾼 비교”가 아니다. 추가 STREAM 분리도 MQTT packet 순서·상태를 정의한 별도 profile이 있어야 한다.

같은 JPEG trace, 생성률, byte budget, 모델/CPU 우선순위, 암호화·인증, broker/subscriber 위치를 고정한다. 각 transport에서 순서와 반복을 교차한다. 동일 메모리 **예산**을 주되, 효율적인 정책의 메모리를 억지로 채우지 않는다.

## 연구 KPI

| KPI | 측정 정의 / 필요한 데이터 |
|---|---|
| 메시지 지연 | `subscriber_receive - publisher_generate`; p50/p95/p99와 deadline 내 도착률. 시계 offset 오차를 함께 보고 |
| 연결 수립 | cold/resumed를 분리한 transport handshake, MQTT CONNACK, 첫 유효 data 시간; pcap 기준과 API 기준 분리 |
| 전달률 / goodput | generated·admitted·sent·unique received를 구분. JPEG는 완성 프레임만 카운트. subscriber 도착 payload bytes / 명시한 관측 구간 |
| 메모리 | internal heap/PSRAM 별 peak·min-free·largest-block, queue payload·descriptor/allocator overhead; allocator 오류·OOM·reboot 횟수 |
| CPU / 추론 | idle/runtime, inference 시간, 실제 capture/AI/frame 수신률; watchdog을 포함. 메모리 사용률로 CPU나 전력을 추정하지 않음 |
| 에너지 | 외부 계측 `∫V(t)I(t)dt`, J/message 및 J/완성 frame, 평균 W. idle/active 측정 경계 명시 |
| wire overhead | 연결 수립·steady·handoff 구간별 pcap bytes/packets, 재전송·ACK 비율. payload goodput과 구분 |
| 핸드오프 복구 | link-down/up, address-change, path-validation, 논리 session, 첫 유효 수신을 기록. 마지막 정상→다음 정상 수신 공백과 link-up→복구를 별도 집계 |
| 최신성 | receiver AoI는 마지막으로 수신한 **가장 최신 생성 시각**을 기준으로 시간 적분. 송신 큐 age·OWDV/jitter를 절대 AoI로 표기하지 않음 |

앞의 논문 KPI에 메모리 압박·영상 최신성·AI 전달을 추가한 프로젝트 정의다. 새 handoff outage duration과 15/30FPS·6MiB 조건은 논문 설정이 아니라 `profiles.json`의 project extension으로 표시한다.

## 현재 도달 상태

- 원문 5편 검토, 저자 코드 commit 확보, 재현 프로필 및 KPI 정의 작성.
- 공통 broker 기준선 구현 및 실제 QoS0/1 메시지 전달 확인.
- 원본 dependency 복원, Pi 에너지 계측, ESP32의 실제 broker/QoS 기준선, loss·cross-AP 반복 실험은 아직 완료하지 않았다.
- 이전 RAM 압박 시험은 유용한 진단 자료지만 MQTT 논문 재현이나 이동성 우위 입증 데이터로 사용하지 않는다.

## 실행한 smoke 결과

최종 코드 검증은 `results/loopback-smoke-03`이다. 100B, 10ms 간격, 100개 × QoS0/1 × 두 transport × 3회: **1200/1200 unique 메시지 수신**, QoS1 PUBACK 및 SUBACK 확인. 아래 SD는 각 실행의 p95 값 사이의 표본 SD이며 pooled p95가 아니다.

| 전송 | QoS | n | 실행별 지연 p95 평균 ± SD(ms) | CONNECT→CONNACK 평균(ms) |
|---|---:|---:|---:|---:|
| quic-stream | 0 | 3 | 1.007 ± 0.171 | 3.861 |
| quic-stream | 1 | 3 | 0.924 ± 0.076 | 5.029 |
| tcp-tls | 0 | 3 | 0.599 ± 0.193 | 1.605 |
| tcp-tls | 1 | 3 | 0.527 ± 0.092 | 1.457 |

무손실 loopback에서는 이 구현의 TCP+TLS가 더 짧은 지연을 보였다. 네트워크 손실·이동성 또는 MCU에서의 일반 우열은 이 수치로 판단하지 않는다. run01은 초기 구현 검증, run02는 `mosquitto -h`의 exit code 3을 실패로 처리한 도구 오류로 측정 전에 중단됐고, 이를 수정한 run03이 최종 검증이다. 원본 논문 재현 실패와 도구 자체 오류를 숨기지 않고 로그에 보존했다.

## 고정 입력·큐 정책 비교 확장

[구현 및 실행](FIXED_REPLAY.md), [통합 결과](FIXED_COMPARISON.md). 저자 MQTT 코드에 고정 입력을 공급하고, ESP32 C admission의 큐 부분 및 H3 현대 이식본을 비교한다. 전체 펌웨어/실제 AP handoff 재현과 구분한다.
