# 논문 KPI 정의를 적용한 호스트 비교

논문 원본 구현·하드웨어·네트워크의 재현 결과가 아니다. MQTT 3.1.1 QoS0/1, Mosquitto 공통 broker, TLS 1.3/TCP 대 aioquic STREAM의 loopback 실험이다.

부하: 10개 × 32B, 100.0ms 간격. Saif Table VII와 Jeddou의 지연 분포/RSD 정의를 참조했다. Saif의 H3 비교군과 NB-IoT emulation은 포함하지 않았다.

표는 실행별 지표의 평균 ± 실행 간 표본 표준편차다. RSD는 각 실행 메시지의 population SD/mean(무차원)이다. raw JSON에는 sample SD 버전도 있다.

| 전송 | QoS | 반복 | 수신/송신 | 지연 평균 ms | p95 ms | RSD | 실행 ms | CPU ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| tcp-tls | 0 | 50 | 500/500 | 4.301 ± 0.078 | 22.015 ± 0.357 | 2.735 ± 0.028 | 902.626 ± 0.465 | 8.668 ± 0.590 |
| tcp-tls | 1 | 50 | 500/500 | 4.303 ± 0.071 | 21.933 ± 0.293 | 2.720 ± 0.037 | 902.795 ± 0.460 | 9.826 ± 0.945 |
| quic-stream | 0 | 50 | 500/500 | 4.440 ± 0.104 | 21.322 ± 0.507 | 2.516 ± 0.057 | 904.814 ± 0.894 | 19.536 ± 2.219 |
| quic-stream | 1 | 50 | 500/500 | 4.434 ± 0.141 | 21.164 ± 0.689 | 2.496 ± 0.048 | 905.052 ± 1.032 | 22.214 ± 2.130 |

CPU 범위는 publisher+subscriber+보안 proxy가 함께 실행되는 Python 프로세스다. Mosquitto는 제외한다. 논문의 단일 클라이언트 CPU 수치와 직접 대조할 수 없다. 실행 시간은 publisher 연결 API 시작부터 마지막 메시지 전달/PUBACK까지다.

## 별도 계측 실행의 누적 메모리 할당

| 전송 | QoS | 반복 | Python 요청 누적 KiB | native 요청 누적 KiB |
|---|---:|---:|---:|---:|
| tcp-tls | 0 | 3 | 23558.826 ± 576.202 | 3350.420 ± 25.148 |
| tcp-tls | 1 | 3 | 36489.486 ± 24.334 | 4315.610 ± 0.001 |
| quic-stream | 0 | 3 | 28689.428 ± 221.628 | 4499.799 ± 380.703 |
| quic-stream | 1 | 3 | 44077.684 ± 45.108 | 4372.339 ± 1.793 |

두 allocator 계층은 겹치므로 합산하지 않는다. 해제된 메모리의 과거 요청도 누적되므로 RAM 최고 점유량이 아니다. Go pprof와 도구/언어 범위가 다르다. 메모리 계측 실행의 지연/CPU 수치는 위 성능 표에 섞지 않았다.

## 첫 메시지 영향 확인

전체 지연 분포에는 새 연결의 첫 메시지도 포함했다. 아래는 원인 분석용 분리이며 논문의 전체 지연 KPI를 대체하지 않는다. 첫 메시지 지연은 packet 기준 TtFDF와 다르다.

| 전송 | QoS | 첫 메시지 지연 평균 ms | 나머지 메시지 지연 평균 ms |
|---|---:|---:|---:|
| tcp-tls | 0 | 39.585 | 0.380 |
| tcp-tls | 1 | 39.418 | 0.402 |
| quic-stream | 0 | 37.937 | 0.718 |
| quic-stream | 1 | 37.621 | 0.746 |

공통 broker/bridge 경로와 새 연결 효과가 포함된 결과다. 특정 전송 계층만의 효과나 ESP32 이동성 우위를 이 표로 입증하지 않는다.

## 미측정 항목

- wire bytes/packets, Initial→첫 PUBLISH: 캡처 권한 없음. `pcap_metrics.py`로 별도 확보한 실제 캡처를 처리할 수 있다.
- 평균 전력/에너지: 전력계 미연결. CPU 시간으로 환산하지 않았다.
- compiled footprint: Python+공유 native 의존성으로 논문의 단일 compiled binary와 불일치.
- ESP32 RAM, 핸드오프 처리량/복구: 이번 실험 대상에 포함되지 않았다.

## 자료

- timing: `experiments/mqtt_quic_papers/results/saif-load10-loopback-timing`
- allocation: `experiments/mqtt_quic_papers/results/saif-load10-loopback-allocation`
- [Saif Table VII](https://arxiv.org/pdf/2208.11178)
- [Jeddou 2022](https://doi.org/10.3390/s22103694)
