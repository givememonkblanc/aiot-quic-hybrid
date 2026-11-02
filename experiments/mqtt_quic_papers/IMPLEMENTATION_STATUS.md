# 논문 구현 비교 진행 상태

2026-09-20. 목표는 같은 입력과 네트워크·메모리 조건에서 실제 구현을 비교하는 것이다. 현재 단계는 원본 계열 빌드 복구와 호스트 기능/부하 검증이며 **전체 논문 구현 및 ESP32 비교가 완료된 상태는 아니다**.

## 비교군과 원본의 관계

| 계열 / 비교군 | 현재 상태 | 남은 차이 |
|---|---|---|
| Fernández 2020/2021 + Jeddou 2022 Paho/VolantMQ | 저자 QUIC fork 및 Go 1.14.15 복원. 실제 MQTT QoS0/1/2 송수신 성공 | draft-era QUIC. Pi/외부 전력/논문 network grid 미실행. 원본 전체 sample은 문법 오류, 필요한 library와 broker만 빌드 |
| Bracht 2026 단일 STREAM | 원고 공개 커밋의 실제 client/broker 실행 | 최신 dependency resolution으로 생성한 Cargo.lock 보존. 원본 dependency lock은 미제공 |
| Bracht 2026 토픽별 STREAM | QoS0 실행, 해당 커밋 QoS1 ACK 오류 확인 | ESP32 미이식, 원고 network grid 미실행 |
| Bracht 2026 PUBLISH별 STREAM | QoS0 실행, 해당 커밋 QoS1 ACK 오류 확인 | 같은 토픽의 MQTT 순서 보장·QoS 상태는 추가 검증 필요 |
| Bracht 2026 DATAGRAM 확장 | QoS0/256B 실송수신 검증 | 표준 STREAM과 전달 의미가 다름. 큰 JPEG를 자동으로 DATAGRAM화하지 않음 |
| 같은 Rust 구현의 TCP+TLS | 실제 broker에서 공통 비교 기준선 실행 | QUIC와 다른 transport 기본값/혼잡제어 영향 포함 |
| Kumar 2019 Google QUIC agent | 미구현 | 논문 구조만 현대 QUIC로 재작성하면 원본 재현으로 부를 수 없음 |
| Saif 2021/2024 H3 및 QUIC tuning | H3 method mapping 현대 이식본 구현, 기능 검증. QUIC tuning 미구현 | 원본 Go 코드와 구분. TLS/Basic 연결 캐시 및 broker-ack POST. tuning 원본 변경분/버전 연결 필요 |
| Alqattaa 2022 gateway | MQTT/QUIC→MQTT/TLS 1.3 현대 이식본 구현 | 저자 코드/원문 전체 설정/0-RTT/실제 cloud는 미확보·미실행. [기능 시험 360/360 전달](GATEWAY_RESULTS.md) |
| Luna 2025 / Tsurumi 2026 | 세부 구현 미확정 | 본문/코드 미확보. 동일 transport의 다른 실험 환경일 수 있음 |
| Piotrovskyi 2026 | 별도 분석 모델군 | QUIC 실측 검증이 없는 모델이므로 구현 벤치마크와 순위 합산하지 않음 |
| 현재 ESP32 hybrid+AMR 정책 | 기존 구현 존재, 이번 matrix에는 미연결 | JPEG DATAGRAM과 MQTT STREAM은 전달 의미가 달라 분리 비교 필요 |

## 복원한 소스와 변경

정확한 commit/바이너리 SHA-256은 `baseline_build_manifest.json`에 있다. 원본 checkout은 유지하고 Paho/VolantMQ 변경은 별도 `*-lab` 사본에 적용했다.

- 저자 `fatimafp95/quic-go` fork를 찾아 개발자 PC의 절대경로 replace를 대체했다. 공개 포크 HEAD가 원문 실험 시점의 정확한 commit인지는 별도 검증이 필요하다.
- Paho가 호출자의 TLSConfig를 무시하고 검증을 끄던 부분을 수정했다. client/server 모두 실험용 인증서로 검증한다.
- VolantMQ 설정이 default port 1883을 함께 남기지 않도록 명시 listener로 교체했다. loopback의 실험용 포트만 사용한다.
- 패치는 `patches/`, Go client harness는 `legacy_client.go`, 재빌드 도구는 `build_baselines.py`에 있다.
- Rust는 원고 공개 commit `8be02fb027bc034ac2c4c14dd4bd5b491a009135`이다. 원본 파일은 변경하지 않았다.
- 원본 Rust의 `quic://`는 인증서 검증을 끄므로 `quics://`로 변경해 실행했다. `author-smoke-01`은 비교에서 제외했고 verified smoke부터 사용한다. 잘못된 CA가 실제 거부되는 negative test도 통과했다.
- 원고 스크립트의 일부 flag(`--inflight` 등)는 해당 커밋 CLI에 없었다. 그대로 실행했다고 하지 않고 지원되는 옵션으로 구성한 로컬 runner를 사용한다.
- 포화 시험에서 원본 HOL 도구의 늦은 warmup 메시지가 본 측정에 들어오는 것을 확인했다. 별도 `mqtt-lib-lab` 사본에서 생성 시각이 측정 시작보다 이전이면 제외하도록 수정했다.
- 원본 HOL 도구는 QoS flag를 publisher에만 적용하고 subscriber는 QoS0으로 구독했다. lab 사본은 같은 QoS로 구독하고 SUBACK과 수신 QoS를 검사한다. 이 때문에 초기 matrix는 진단 자료이며 종단 QoS1 비교 결과로 사용하지 않는다. 수정 내용은 `patches/mqtt-lib-measurement.patch`로 보존한다.

## 실행 방법

```bash
python3 experiments/mqtt_quic_papers/build_baselines.py --family all
python3 experiments/mqtt_quic_papers/compare_legacy.py \
  --count 100 --repetitions 3 --output /tmp/legacy-mqtt-run
python3 experiments/mqtt_quic_papers/compare_author.py \
  --duration 5 --repetitions 3 --qos 0 1 --payloads 256 32768 --rate 30 \
  --output /tmp/author-mqtt-run
python3 experiments/mqtt_quic_papers/author_report.py /tmp/author-mqtt-run \
  --output /tmp/author-mqtt-report.md
```

원본 코드와 compiler 다운로드는 실험 디렉터리에만 둔다. Go archive SHA-256을 확인하고 Cargo dependency lock을 고정한다. builder는 다른 commit의 기존 checkout을 강제로 reset하지 않는다. 다른 호스트의 toolchain 버전까지 자동 변경하지 않으며 Rust 최소 버전을 만족해야 한다.

## 현재 결과 해석

[정상 입력률 결과](AUTHOR_BASELINE_RESULTS.md)는 4가지 transport/stream 전략 × QoS0/1 × 256B/32768B × 3회, 총 48회다. 4개 토픽 합계 명목 30 message/s, warmup 1초, 측정 2초다. publisher와 subscriber의 QoS를 맞추고 warmup 경계를 수정한 실행이다. 이 값들은 우리 로컬 시험 설정이며 원고 전체 실험 조건이 아니다.

- QoS0은 24/24회 성공했다. QoS1은 TCP+TLS와 control-only의 12/12회만 성공했다. 성공한 36회는 2,160/2,160 unique 메시지를 수신했다.
- QoS1 per-topic/per-publish는 12/12회 실패했고 수신 callback에 도착한 메시지가 0이었다. 로그에 `QUIC write error: sending stopped by peer: error 0`이 남는다.
- 해당 커밋의 `broker/server_stream_manager.rs`는 서버가 연 양방향 스트림의 수신 핸들 `_recv`를 유지하지 않는다. client reader는 그 스트림으로 PUBACK을 보내려 하므로 ACK 경로 오류와 부합한다. 저자 transport를 수정해 성공으로 바꾸지 않고 원본의 실패로 기록했다. 다른 버전이나 모든 MQTT/QUIC 구현의 결함이라는 의미는 아니다.
- 이전 `author-matrix-01`과 [초기 진단 표](AUTHOR_BASELINE_DIAGNOSTIC.md)는 subscriber가 QoS0이었던 결과다. 종단 QoS1 성능 근거에서 제외한다. `author-saturation-01`도 warmup 혼입으로 비교에서 제외하고 각 디렉터리에 `audit.json`을 남겼다.
- 구형 Paho/VolantMQ의 별도 기능 검증은 256B, QoS0/1/2, 100개씩 3회로 **900/900 수신**했다. Rust와 동일 성능 시험은 아니므로 지연 순위를 합치지 않는다.

같은 프로그램에서 strategy를 바꿔 비교했지만 호스트는 전용 실험 장비로 격리되지 않았다. 반복 3회, 짧은 loopback 측정으로 미세한 우열을 확정하지 않는다. CPU의 0ms는 `/proc` 누적 CPU tick에서 증가를 관측하지 못했다는 뜻이며 연산이 없다는 뜻이 아니다.

포화 실험은 rate=0으로 생성 가능한 만큼 보내는 별도 workload다. 동일 생성량 시험과 혼합하지 않는다. 저자 도구가 전송 후 기다리는 drain은 500ms이므로, 미수신 메시지가 있으면 측정 종료 시 미도착/큐 잔류일 수 있다. 패킷 손실이라고 단정하지 않는다. 수정한 도구로 재실행한 결과는 [포화 결과](AUTHOR_SATURATION_RESULTS.md)에 별도로 기록한다.

수정 후 포화 시험은 QoS0/32KiB, 측정 5초, 3회씩 총 12회다. warmup 혼입으로 생겼던 중복은 0개로 해소됐다. control-only 380,111/380,111, per-topic 371,347/371,347, TCP+TLS 1,203,870/1,203,870을 수신했다. per-publish는 301,315/368,149(81.85%) 수신했고 broker peak RSS의 실행별 평균은 약 496.9MiB로 가장 컸다. per-publish의 종료 시 미도착은 66,834개다. 최대 속도 생성 시험이라 방식별 송신량이 다르므로 동일 부하 성능 순위가 아니다.

포화 시 모든 방식의 callback trace에서 sequence 역전이 보였다. 원본 `callback.rs`는 callback마다 `tokio::spawn`을 호출하므로 이 관측에는 callback 실행 순서가 포함된다. wire 수신 순서나 MQTT 프로토콜 순서 위반으로 단정하지 않으며, 지연도 callback 도착까지의 애플리케이션 지연이다.

현재 제한은 host 프로세스별 2GiB 가상주소 공간이며 ESP32 RAM 예산을 재현한 것이 아니다. broker RSS와 client RSS를 따로 측정한다. Go TotalAlloc과 Rust RSS는 같은 메모리 KPI로 직접 비교하지 않는다.

## 동일 생성 입력 비교 확장

[FIXED_REPLAY.md](FIXED_REPLAY.md)에 실제 저자 MQTT transport와 H3 이식본, ESP32 C admission의 큐 전용 분리 실험을 연결했다. 결과는 [FIXED_COMPARISON.md](FIXED_COMPARISON.md)에 별도로 기록한다. 기존 전송 후 sleep 대신 고정 입력 스케줄을 사용한다. 이번 pause는 application 송신 중단이며 실제 network/AP handoff가 아니다. 별도의 [NAT 재바인딩 시험](REBIND_RESULTS.md)은 실제 UDP 폐기/source-port 변경과 저자 migrate() 호출을 비교하며 AP 실기 시험과 구분한다.

## ESP32 최적 방식 판정에 남은 필수 작업

1. 실제 MQTT publisher→broker→subscriber 경로를 ESP32 측정에 연결하고 동일 입력 trace로 정책을 비교한다.
2. 같은 MCU 메모리 예산, AI 부하, 생성률을 고정한다. admission·deadline 폐기·QoS의 차이를 명시한다.
3. 별도 실험 링크에서 지연·손실·대역폭·단절을 통제한다. 현 계정의 packet capture 권한은 없으며 관리 중인 host NIC에는 임의 tc 규칙을 적용하지 않았다.
4. 실제 AP 핸드오프와 NAT/IP 변경을 구분한다. 연결 생존, 수신 공백, 복구, 최신 프레임 deadline을 측정한다.
5. 수신 신뢰성·오류·RAM 예산 조건을 먼저 만족한 후보 중 deadline/지연·CPU·전력의 상충 관계를 평가한다. 평균 FPS만으로 전체 우승자를 선정하지 않는다.

## 이번 확장 실행 집계

- 고정 입력 matrix 100회: 각 180개, 총 18,000개 생성. 모든 실행 종료, 중복 및 payload 검증 오류 0개. 폐기/만료는 별도 기록하며 수신 100%를 뜻하지 않는다. 생성 시각의 최대 지연은 4.491ms였다.
- H3 기능 시험: method mapping/구독 전달, Basic 인증 및 연결 캐시, 틀린 자격 증명과 CA 거부를 검증했다.
- gateway 기능 시험: direct TLS와 QUIC→TLS 경로 × QoS0/1 × 3회, 총 360/360 메시지 전달. gateway→broker 12개 연결은 TLSv1.3, gateway 오류 0개.
- 실제 보드는 read-only RAM telemetry에서 mode 2 / policy 1 / 목표 30FPS / 큐 예산 6MiB를 확인했다. 이 관측은 새로운 ESP32 replay/handoff 측정이 아니다.
- 실제 AP 간 반복 시험은 두 번째 AP 사용 가능 여부 확인이 필요하다. 동시에 ESP32의 공통 MQTT/replay 경로 연결도 남아 있다. host 결과만으로 보드의 최적 방식을 선언하지 않는다.

## NAT 재바인딩 결과와 현재 판단

NAT fixture는 4개 큐 정책 × 정상/수동 재바인딩/명시 migration × 2회 = 24회다. 같은 입력 스케줄의 고정 입력 시험 100회와 합계 124회/22,320개 생성 기록, source snapshot 및 입력 hash를 대조했다. `results/fixed-suite-audit.json` 참조.

- 수동 재바인딩: 모든 큐 정책에서 8초 관측 내 subscriber의 새 데이터 수신은 복구되지 않았다. publisher의 QUIC 연결과 publish API 성공만으로 종단 복구를 판정할 수 없다.
- 양쪽 client의 명시 migration: 8/8회 원래 QUIC 연결 객체 유지 + 양쪽 경로 검증 + 새 입력 수신을 확인했다. 재연결이나 새 TLS 세션으로 대체한 결과가 아니다.
- 이 호스트 조건에서 새 입력 수신 재개 평균은 FIFO 약 379ms, deadline 약 55ms, latest 약 196ms, AMR queue-only 약 383ms였다. 각각 반복 2회의 탐색 결과라 전역 우열은 미판정이다.
- application queue 최고점은 FIFO/deadline 6MiB, latest 1.25MiB, AMR queue-only 0.25MiB였다. AMR 큐 절감은 크지만 복구 지연이 가장 작지는 않았다. latest의 run 간 변동도 커 후속 반복이 필요하다.

현재의 설계 근거는 **연결 유지, 능동 경로 갱신, deadline 만료, 큐 admission을 별도 축으로 비교해야 한다**는 것이다. ESP32 전체 RAM·AI 동시 부하·실제 AP 전환으로 확인하기 전에는 이 결과를 MCU의 최종 승자로 일반화하지 않는다.
