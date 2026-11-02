# 동일 생성 입력 비교 도구

## 구현과 범위

`compare_fixed.py`는 전달 속도와 무관하게 **정해진 시각에 정해진 수의 입력**을 생성한다. 기존 저자 benchmark의 전송 완료 후 sleep 방식과 다르다. 입력 sequence, 예정 시각, 실제 생성 시각, admission/폐기 사유, publish API 결과, subscriber 수신을 모두 보존한다. 원본 MQTT transport는 수정하지 않고 별도 client harness를 사용한다.

- MQTT: Bracht 공개 커밋의 실제 client와 원본 broker. TCP+TLS, control-only, per-topic, per-publish. 인증서 검증을 켠다.
- H3: `h3_pubsub.py`와 `h3_fixed_client.py`. Saif 2021 Table I / 2023 저자 수정본 Table II의 PUT/HEAD/DELETE/POST/GET 구조를 현대 Python/aioquic으로 이식했다. POST HTTP 200은 broker ACK이며 subscriber 전달 ACK나 MQTT QoS1이라고 부르지 않는다. 구독 GET의 body에는 자체 4-byte 길이 prefix를 사용한다. 원본 Go/Drift 코드나 wire 형식 재현은 아니다.
- H3는 TLS 서버 인증, 선택적 Basic 인증 및 연결 단위 인증 캐시를 구현했다. 비교 matrix의 익명 client는 Rust baseline과 맞춘 설정이다. Basic 인증·캐시는 별도 기능 시험만 수행한다. Saif의 QUIC timer/ACK/MAX_STREAMS/RequestWriter tuning은 아직 구현하지 않았다.
- gateway: `gateway_compare.py`가 실제 MQTT/QUIC→gateway→MQTT/TLS 1.3→Mosquitto 경로와 직접 MQTT/TLS를 시험한다. 서버 인증은 두 암호화 구간에서 수행한다. Alqattaa 구조의 이식본이며 저자 코드/실제 cloud/0-RTT 재현이 아니다. gateway의 기능 시험은 고정 재생 matrix와 따로 둔다.

## 큐 정책

| 정책 | 구현 |
|---|---|
| fifo | 같은 payload budget까지 보관. 만료 없음 |
| deadline | FIFO 저장 후 publish 직전에 2초 만료 적용 |
| latest | 같은 토픽의 아직 대기 중인 객체를 새 객체로 교체. 진행 중 publish는 취소하지 않음. 2초 만료 적용 |
| amr_latest_queue_only | ESP32의 실제 `video_admission.c` policy 1을 C library로 링크해 호출. 대기/진행 중 payload가 있으면 admission 거부. 2초 만료 적용 |

AMR 분리 실험에는 QUIC RTT/cwnd/in-flight/RSSI를 전달하지 않는다. 0 값은 원본 C의 미계측 fallback이며 임의 측정값을 만들지 않는다. 따라서 **전체 ESP32 스케줄러 이식 완료가 아니다**. actual in-flight gate, PSRAM free guard, JPEG 조각, AI 우선순위, migration/reconnect는 추가 연결이 필요하다.

모든 정책은 동일한 application payload budget을 갖는다. 실제 heap allocation한 payload와 진행 중 publish를 센다. MQTT/QUIC 내부 복사본·buffer 및 allocator overhead는 이 예산 밖이므로 client/broker RSS도 별도로 측정한다. 예산 6MiB를 전체 프로세스/ESP32 RAM 제한으로 설명하지 않는다.

## 현재 workload

- 30개/초 × 6초 = 각 실행 180개, 각 payload 256KiB, 4개 토픽 순환.
- 일정한 합성 바이트 패턴과 sequence/예정시각 header. 사람 사진이나 기존 영상을 복사하지 않는다.
- 모든 실행에서 입력 스케줄 SHA-256 동일. 생성기는 송신 worker와 독립적이다.
- 정상과 application 송신 pause를 구분한다. pause는 t=1초부터 3초간 송신 worker를 멈춘다. 진행 중 publish는 취소하지 않는다. 생성기는 계속 실행한다.
- **pause는 AP handoff, IP 변경, packet loss, path validation 또는 QUIC migration이 아니다.** private network namespace 생성은 현 계정에서 거부됐다. host NIC나 관리 네트워크를 변경하지 않았다.
- deadline 2초, 생성 구간 뒤 관측 drain 2초. 생성률을 맞췄으므로 drop된 메시지도 전달률/deadline 분모에 포함한다.
- publisher/subscriber는 같은 프로세스의 monotonic clock을 공유한다. 지연은 예정 생성 시각부터 subscriber callback까지이며 생성 지연과 callback scheduling도 포함한다.
- 각 실행의 순서를 두 번째 반복에서 뒤집는다. CPU/메모리 값은 host 탐색 실험이며 MCU 실측과 합산하지 않는다.

## 실행

먼저 기존 `build_baselines.py --family modern`으로 고정 커밋과 의존성을 복원한다.

```bash
python3 experiments/mqtt_quic_papers/build_fixed.py
python3 experiments/mqtt_quic_papers/compare_fixed.py \
  --output /tmp/fixed-q0 --repetitions 2
python3 experiments/mqtt_quic_papers/compare_fixed.py \
  --output /tmp/fixed-ack --cases tcp-tls control-only h3-port --qos 1 --repetitions 2
python3 experiments/mqtt_quic_papers/fixed_report.py /tmp/fixed-q0 /tmp/fixed-ack \
  --output /tmp/fixed-report.md
experiments/mqtt_quic_papers/.venv/bin/python experiments/mqtt_quic_papers/gateway_compare.py \
  --output /tmp/gateway-functional
```

H3의 `--qos 1`은 공통 도구에서 broker-ack 프로필을 선택하기 위한 flag다. 결과표에는 MQTT QoS1이 아닌 `HTTP 200`으로 표시한다. H3에 `--qos 0`을 주면 거부한다. 실패한 저자 multistream QoS1은 앞선 원본 비교에서 제외하지 않고 기록했으며, 정상 동작하는 QoS1 TCP/단일 STREAM과 H3 ACK를 별도 matrix로 실행한다.

## 검증과 결과 파일

- `fixed_build_manifest.json`: compiler, source, C bridge, Cargo.lock, 실행 바이너리/공유 라이브러리 해시.
- 각 결과 디렉터리의 `input_schedule.json`, `manifest.json`, `sources/`, case별 `config.json`, `commands.json`, `raw.json`, `resources.json`, `result.json`.
- `fixed_metrics.py`: 일정 변경·누락 생성·QoS 불일치·수신 시각 오류를 거부. 중복은 첫 수신만 집계. `test_fixed_metrics.py`에서 누락/late 메시지가 deadline 분모에서 사라지지 않는지 확인.
- `test_h3_pubsub.py`: HTTP method mapping, 큰 body 구독 전달, 인증 캐시, 틀린 자격 증명 및 틀린 CA 거부.
- 결과는 [FIXED_COMPARISON.md](FIXED_COMPARISON.md)에 통합한다. 실험 환경상 전체 최적 방식이나 ESP32 우승자는 아직 판정하지 않는다.

## 남은 실기 연결

실제 ESP32에 동일 생성 trace를 공급하는 연구 모드, MQTT broker fan-out/QoS1 경로, RAM/PSRAM 및 AI 부하 동시 측정이 필요하다. 두 번째 AP가 사용 가능한지 확인한 후 실제 AP 전환 구간과 수신 기록을 연결해야 한다. 현재 보드의 live 영상 통계와 이번 synthetic host matrix를 동일 조건 결과처럼 합치지 않는다.

## 실제 QUIC NAT 재바인딩 fixture

`udp_rebind.py`는 loopback UDP 양방향 datagram을 일정 시간 실제 폐기한 뒤 broker 쪽 source port를 바꾼다. `udp-pass` 대조군도 같은 중계를 지난다. host NIC, 라우팅, 방화벽 설정은 바꾸지 않는다. application pause와 별개의 실제 transport 시험이지만 RF/AP 핸드오프나 ESP32 실기는 아니다.

`--active-migration`을 주면 network 복구 예정 시각 +20ms에 publisher와 subscriber 양쪽에서 원본 MQTT library의 `migrate()`를 호출한다. QUIC 객체 ID 유지, 종료 원인 없음, 양쪽 PATH_CHALLENGE/RESPONSE, 복구 이후 생성 메시지 수신을 함께 검사한다. 단순 publish 성공을 subscriber 복구로 해석하지 않는다. 이 trigger 시각은 fixture가 제공하며 실기 Wi-Fi 이벤트 검출기의 성능을 포함하지 않는다.

```bash
python3 experiments/mqtt_quic_papers/compare_fixed.py \
  --output /tmp/rebind-control --cases control-only --scenarios normal --qos 1 --network udp-pass
python3 experiments/mqtt_quic_papers/compare_fixed.py \
  --output /tmp/rebind-passive --cases control-only --scenarios normal --qos 1 --network udp-rebind
python3 experiments/mqtt_quic_papers/compare_fixed.py \
  --output /tmp/rebind-active --cases control-only --scenarios normal --qos 1 --network udp-rebind --active-migration
python3 experiments/mqtt_quic_papers/rebind_report.py /tmp/rebind-control /tmp/rebind-passive /tmp/rebind-active \
  --output /tmp/rebind-report.md
```

결과: [REBIND_RESULTS.md](REBIND_RESULTS.md). gateway 기능 시험은 [GATEWAY_RESULTS.md](GATEWAY_RESULTS.md) 참조.
