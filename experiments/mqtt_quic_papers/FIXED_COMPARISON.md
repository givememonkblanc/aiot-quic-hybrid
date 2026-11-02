# 고정 생성 입력 비교

각 실행 6초 × 30개/초, payload 262144B, 큐 payload 예산 6291456B, deadline 2000ms, drain 2000ms.
실제 MQTT/H3 네트워크 송수신이다. 입력은 동일한 결정적 합성 바이트이며 카메라 JPEG·AI 부하가 아니다.
pause는 시작 1000ms 뒤 송신 worker를 3000ms 멈춘다. AP 전환·패킷 손실·QUIC migration을 재현하지 않는다.

| 조건 | 전송 | ACK 프로필 | 큐 정책 | 실행 완료/시도 | unique/생성 | deadline 충족률 | 수신 p95 평균 ms | 큐 최고 MiB | client RSS 최고 MiB |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| normal | control-only | MQTT QoS0 | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 3.26 | 0.25 | 6.80 |
| normal | control-only | MQTT QoS0 | deadline | 2/2 | 360/360 | 100.0% | 3.30 | 0.25 | 6.71 |
| normal | control-only | MQTT QoS0 | fifo | 2/2 | 360/360 | 100.0% | 3.20 | 0.25 | 7.54 |
| normal | control-only | MQTT QoS0 | latest | 2/2 | 360/360 | 100.0% | 3.31 | 0.25 | 7.35 |
| normal | control-only | MQTT QoS1 | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 3.13 | 0.25 | 50.31 |
| normal | control-only | MQTT QoS1 | deadline | 2/2 | 360/360 | 100.0% | 3.26 | 0.25 | 50.41 |
| normal | control-only | MQTT QoS1 | fifo | 2/2 | 360/360 | 100.0% | 3.27 | 0.25 | 50.41 |
| normal | h3-port | HTTP 200 | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 17.86 | 0.25 | 50.40 |
| normal | h3-port | HTTP 200 | deadline | 2/2 | 360/360 | 100.0% | 18.03 | 0.50 | 51.68 |
| normal | h3-port | HTTP 200 | fifo | 2/2 | 360/360 | 100.0% | 16.92 | 0.25 | 49.21 |
| normal | per-publish | MQTT QoS0 | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 3.54 | 0.25 | 6.55 |
| normal | per-publish | MQTT QoS0 | deadline | 2/2 | 360/360 | 100.0% | 3.45 | 0.25 | 7.12 |
| normal | per-publish | MQTT QoS0 | fifo | 2/2 | 360/360 | 100.0% | 3.42 | 0.25 | 7.14 |
| normal | per-publish | MQTT QoS0 | latest | 2/2 | 360/360 | 100.0% | 3.37 | 0.25 | 6.93 |
| normal | per-topic | MQTT QoS0 | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 3.13 | 0.25 | 9.07 |
| normal | per-topic | MQTT QoS0 | deadline | 2/2 | 360/360 | 100.0% | 3.14 | 0.25 | 7.23 |
| normal | per-topic | MQTT QoS0 | fifo | 2/2 | 360/360 | 100.0% | 3.32 | 0.25 | 7.56 |
| normal | per-topic | MQTT QoS0 | latest | 2/2 | 360/360 | 100.0% | 3.23 | 0.25 | 8.09 |
| normal | tcp-tls | MQTT QoS0 | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 2.64 | 0.25 | 6.43 |
| normal | tcp-tls | MQTT QoS0 | deadline | 2/2 | 360/360 | 100.0% | 2.52 | 0.25 | 6.14 |
| normal | tcp-tls | MQTT QoS0 | fifo | 2/2 | 360/360 | 100.0% | 2.56 | 0.25 | 6.62 |
| normal | tcp-tls | MQTT QoS0 | latest | 2/2 | 360/360 | 100.0% | 2.55 | 0.25 | 6.54 |
| normal | tcp-tls | MQTT QoS1 | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 2.76 | 0.25 | 50.02 |
| normal | tcp-tls | MQTT QoS1 | deadline | 2/2 | 360/360 | 100.0% | 2.73 | 0.25 | 49.86 |
| normal | tcp-tls | MQTT QoS1 | fifo | 2/2 | 360/360 | 100.0% | 2.80 | 0.25 | 50.76 |
| pause | control-only | MQTT QoS0 | amr_latest_queue_only | 2/2 | 180/360 | 50.0% | 3.15 | 0.25 | 6.90 |
| pause | control-only | MQTT QoS0 | deadline | 2/2 | 180/360 | 50.0% | 3.09 | 6.00 | 12.94 |
| pause | control-only | MQTT QoS0 | fifo | 2/2 | 226/360 | 49.4% | 2819.23 | 6.00 | 12.07 |
| pause | control-only | MQTT QoS0 | latest | 2/2 | 188/360 | 52.2% | 3.75 | 1.25 | 7.96 |
| pause | control-only | MQTT QoS1 | amr_latest_queue_only | 2/2 | 180/360 | 50.0% | 3.10 | 0.25 | 29.16 |
| pause | control-only | MQTT QoS1 | deadline | 2/2 | 179/360 | 49.7% | 3.20 | 6.00 | 28.67 |
| pause | control-only | MQTT QoS1 | fifo | 2/2 | 226/360 | 49.4% | 2818.67 | 6.00 | 33.91 |
| pause | h3-port | HTTP 200 | amr_latest_queue_only | 2/2 | 180/360 | 50.0% | 17.52 | 0.25 | 49.70 |
| pause | h3-port | HTTP 200 | deadline | 2/2 | 180/360 | 50.0% | 17.72 | 6.00 | 54.40 |
| pause | h3-port | HTTP 200 | fifo | 2/2 | 226/360 | 49.4% | 2899.61 | 6.00 | 54.64 |
| pause | per-publish | MQTT QoS0 | amr_latest_queue_only | 2/2 | 180/360 | 50.0% | 3.32 | 0.25 | 6.77 |
| pause | per-publish | MQTT QoS0 | deadline | 2/2 | 180/360 | 50.0% | 3.68 | 6.00 | 12.36 |
| pause | per-publish | MQTT QoS0 | fifo | 2/2 | 226/360 | 49.4% | 2819.99 | 6.00 | 12.52 |
| pause | per-publish | MQTT QoS0 | latest | 2/2 | 188/360 | 52.2% | 4.07 | 1.25 | 7.68 |
| pause | per-topic | MQTT QoS0 | amr_latest_queue_only | 2/2 | 180/360 | 50.0% | 3.49 | 0.25 | 7.52 |
| pause | per-topic | MQTT QoS0 | deadline | 2/2 | 180/360 | 50.0% | 3.45 | 6.00 | 13.58 |
| pause | per-topic | MQTT QoS0 | fifo | 2/2 | 226/360 | 49.4% | 2817.71 | 6.00 | 13.25 |
| pause | per-topic | MQTT QoS0 | latest | 2/2 | 188/360 | 52.2% | 4.40 | 1.25 | 8.02 |
| pause | tcp-tls | MQTT QoS0 | amr_latest_queue_only | 2/2 | 180/360 | 50.0% | 2.27 | 0.25 | 6.98 |
| pause | tcp-tls | MQTT QoS0 | deadline | 2/2 | 180/360 | 50.0% | 2.48 | 6.00 | 11.68 |
| pause | tcp-tls | MQTT QoS0 | fifo | 2/2 | 226/360 | 49.4% | 2817.14 | 6.00 | 12.44 |
| pause | tcp-tls | MQTT QoS0 | latest | 2/2 | 188/360 | 52.2% | 3.16 | 1.25 | 7.51 |
| pause | tcp-tls | MQTT QoS1 | amr_latest_queue_only | 2/2 | 179/360 | 49.7% | 2.68 | 0.25 | 26.81 |
| pause | tcp-tls | MQTT QoS1 | deadline | 2/2 | 180/360 | 50.0% | 2.76 | 6.00 | 27.88 |
| pause | tcp-tls | MQTT QoS1 | fifo | 2/2 | 226/360 | 49.4% | 2816.41 | 6.00 | 33.53 |

## 해석 범위

- 모든 실행에 동일한 생성 스케줄을 주며 폐기된 메시지도 deadline 분모에 포함한다. 수신된 메시지만의 p95로 전체 성능을 평가하지 않는다.
- 입력 시각 지연은 각 result에 따로 기록한다. source·바이너리·입력 SHA-256 및 raw 생성/제출/수신 기록을 보존한다.
- `amr_latest_queue_only`는 ESP32의 실제 video_admission.c policy 1을 호출한다. RTT/cwnd/in-flight/RSSI를 모르는 기본 경로만 사용한 큐 admission 분리 실험이다. 전체 펌웨어 정책, AI 우선 처리, PSRAM guard, QUIC migration은 포함하지 않는다.
- FIFO는 만료가 없고 deadline/AMR은 2초 만료를 적용한다. 그러므로 이는 transport와 큐 정책의 별도 축이며, FIFO 대비 차이를 QUIC만의 효과라고 부르지 않는다.
- 큐 예산은 application payload + 진행 중 publish를 센다. MQTT/QUIC 라이브러리와 broker 내부 버퍼, allocator overhead는 별도다. 전체 ESP32 RAM 6MiB 제한을 달성한 결과가 아니다.
- H3는 Saif의 HTTP method mapping을 Python/aioquic으로 구현한 별도 비교군이다. 원본 Go 코드·QUIC tuning 재현이 아니며 HTTP 200은 broker 처리 ACK로 MQTT QoS1과 같은 프로토콜이 아니다. 구현 언어 차이가 있으므로 CPU/RSS 차이를 프로토콜 고유 효과로 단정하지 않는다.
- H3와 MQTT 모두 이번 표에서는 서버 인증 TLS + 익명 client를 사용한다. H3 Basic 인증/연결 캐시는 별도 기능 시험으로 확인한다.
- 서버와 subscriber는 모두 loopback이다. 실험 장비는 다른 작업과 완전히 격리되지 않았고 반복은 2회이므로 미세 지연 차이의 통계적 우열을 주장하지 않는다.
- 결론 범위는 고정 입력 및 application 송신 정지에 대한 기능·큐 동작이다. 실제 AP 핸드오프 수신 공백/복구 시간, 전력, ESP32 최적 방식은 미판정이다.

원자료: experiments/mqtt_quic_papers/results/fixed-matrix-01, experiments/mqtt_quic_papers/results/fixed-ack-matrix-01, experiments/mqtt_quic_papers/results/fixed-latest-matrix-01
