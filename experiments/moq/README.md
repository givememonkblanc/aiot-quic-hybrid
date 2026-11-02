# Media over QUIC 전환

소스 구조와 현재 실행 진입점은 [CODE_MAP.md](CODE_MAP.md)를 참고한다.
펌웨어 MoQ 옵션은 각 컴포넌트의 `moq_options.cmake`, 호스트 테스트는
`tests/`, 문헌 재현 작업은 `literature/`로 구분한다.
검증 명령은 `bash experiments/moq/run_checks.sh`이다.
아래 내용은 전환 당시부터의 실험 기록이며, 최신 결과는
[연구 상태](../../papers/mission_aiot_moq/DEVELOPMENT_STATUS.md)와
[문헌 비교 계약](literature/COMPARISON_CONTRACT.md)에서 확인한다.

2026-09-22 사용자 결정. 상태: native QUIC 단일 JPEG 트랙의 제한된 구현으로 ESP32 실물 전송·정상 종료 검증 완료. [측정 결과](RESULTS.md): 39회 제출 시도 중30개 수락,30개 해시 일치 수신,9개 슬롯 상한 거절. [구현 범위·재현 방법](IMPLEMENTATION.md)을 참고한다. [후속 두 트랙 시험](CONCURRENT.md)에서는 ESPDet 동시 실행·평균9.95Hz 수집·AI 결과27건 수신을 확인했다. 최대 수집 간격315ms와 영상15/598장 수신이 남은 한계다. 전체 MOQT 상호운용이나 임무 QoS 개선을 뜻하지 않는다.

## 무엇을 바꾸는가

기존 ESP32 JPEG application fragmentation + 자체 track 번호와 헤더를 사용하는 미디어 경로를 MOQT publisher/subscriber 경로로 전환한다. 하부 QUIC/TLS는 유지한다. 임의의 헤더에 track/group/object 번호만 붙이는 것을 표준 MOQT 구현이라고 부르지 않는다.

프로토콜 기준은 [IETF draft-ietf-moq-transport-21](https://datatracker.ietf.org/doc/html/draft-ietf-moq-transport-21)로 고정한다. 확인일2026-09-22, 문서 발행2026-09-08. 아직 Internet-Draft이므로 peer 구현의 지원 버전을 확인하고 정확히 일치시킨다. 다른 구현의 moq-lite나 다른 draft와 호환된다고 가정하지 않는다. Native QUIC와 WebTransport 중 ESP32–서버에는 native QUIC를 선택한다. 브라우저 연동은 별도의 edge gateway 단계다.

## 기존 코드에서 확인한 사항

- `src/components/transport/moqt_http3_client.cpp`는 실제 연결/구독/전송을 구현하지 않은 미완성 코드다. 연결 함수는 Disconnect를 호출한 후 connected=true로 설정하고, publish는 payload를 전송하지 않는다.
- 해당 파일은 transport CMake 소스 목록에도 없다. 동작하는 MOQT 기반으로 간주하지 않는다.
- 현재 ngtcp2 어댑터에는 MQTT용 stream 읽기/쓰기 경로와 DATAGRAM 전송이 있다. MOQT 요청 및 독립적인 미디어 stream 생명주기를 지원하는 어댑터 확장이 필요하다.
- 기존 picoquic 수신기는 자체 JPEG/AI framing을 처리한다. ALPN만 바꾸면 되는 수신기가 아니다.
- 현장 보드는 기존 FOMO 펌웨어이며 이 방향 변경으로 flash하거나 네트워크 설정을 변경하지 않았다.

## 첫 번째 응용 매핑 (설계안)

Namespace는 장치별로 분리하고 트랙의 역할과 schema version을 명시한다. 아래 이름은 응용 설계이며 표준 내장 미디어 포맷이라는 뜻이 아니다.

| Track | 객체 단위 | 응용 요구 |
|---|---|---|
| preview/jpeg | 완전한 JPEG 프레임1장 | 제한된 freshness, frame 단위 완료/폐기 |
| ai/events | event1건 | event ID, 원본 capture ID/시각, class/confidence, deadline |
| evidence/jpeg | event와 연결된 JPEG1장 | event ID 연결, 완성 여부, 필요한 증거의 기한 |

JPEG는 프레임 간 예측 의존성이 없으므로 초기안은 프레임별 독립 group, object0으로 둔다. 객체는 subgroup stream으로 전송하고 draft의 종료/취소 규칙에 맞춰 수명을 관리한다. 큰 JPEG를 한 DATAGRAM에 넣거나, 기존1KB 조각을 별개 프레임 객체로 취급하지 않는다. MTU에 따른 QUIC packetization과 응용 객체 경계를 구분한다. 미디어 포맷 식별·타임스탬프·카탈로그 방식은 해당 mapping specification과 peer 호환 검증을 통해 별도로 확정한다.

AI 이벤트의 application deadline은 QUIC 신뢰성이나 MOQT timeout만으로 보장되지 않는다. 폐기·취소·미구독·수신 실패를 모두 기록한다. 증거 이미지가 필수인 임무는 이벤트와 이미지가 모두 완성돼야 성공이다. 미리보기가 꼭 필요하지 않은 임무에서는 preview FPS를 임무 성공과 혼동하지 않는다.

OTA/프로비저닝 등의 장치 제어는 미디어 이식의 첫 단계와 분리한다. 기존 MQTT 운영 경로를 먼저 보존하고, 동일 connection에서 서로 다른 application protocol을 임의 혼합하지 않는다. 병행 연결을 사용할 경우 그 RAM/CPU 비용도 측정한다.

## 구현 순서와 완료 조건

1. **Native QUIC 세션과 구독:** draft21 wire codec, bounds-checked incremental parsing, version/ALPN, SETUP, 요청 stream/구독 lifecycle. 독립 peer와 성공/거절/종료를 확인한다. 함수 return0만으로 성공 처리하지 않는다.
2. **JPEG1장 end-to-end:** 구독한 트랙의 object를 송신하고 수신측에서 길이·해시·JPEG decode를 확인한다. 연결/구독 실패 시 객체가 전송된 것으로 집계되지 않아야 한다.
3. **연속 capture와 bounded object lifetime:** capture10Hz를 영상 송신 성공과 분리한다. queue cap, stream cancellation, 미완성 객체 정리, disconnect/reconnect 후 중복/메모리 회수를 확인한다.
4. **AI/event와 evidence 연결:** 공통 event/capture ID, 발생·추론·송신·수신 시각을 기록한다. receiver 화면의 최신 이미지가 이벤트 증거를 잘못 대신하지 않게 한다.
5. **동일 입력/열화 조건 평가:** 기존 방식, 고정 MoQ 정책, 필요할 경우 적응형 MoQ 정책을 비교한다. framing·신뢰성·payload·TLS·자원 예산 차이를 명시한다.
6. **실제 약전계 및 AP 전환:** RSSI/PHY/MAC 재전송을 동반하는 실험은 host netem 재현과 구분한다.

완료라 부를 첫 기준은 **ESP32 publisher → MOQT subscriber에서 구독이 성립하고 실제 JPEG 객체가 검증되는 것**이다. 이후 보드의 안정성과10Hz 수집, mission QoS를 검증한다. 문서 변경이나 호스트 codec 시험만으로 전환 완료라고 하지 않는다.

## 연구 질문과 비교 기준

MoQ 도입 자체가 CPU 병목이나 대역폭 부족을 없앤다고 가정하지 않는다. 연구 후보는 약한 링크에서 제한된 MCU가 어떤 객체를 어떤 크기·순서·기한으로 전달해야 임무를 가장 잘 수행하는가이다. 먼저 표준/고정 정책의 성능을 측정하고 추가 알고리즘 필요성을 판단한다.

- Primary: 실제 event 전체를 분모로 하는 올바른 deadline 내 임무 완수율, 오탐, 필수 evidence 완성률.
- Secondary: 정보 age/갱신 공백, 완성 JPEG goodput, event/객체별 admission/drop/expiry/reset 원인.
- Constraints: capture>=10Hz에 대한 구간별 통계 및 별도 inter-frame deadline, RAM 한도, watchdog/reset, AI 품질 하한.
- Diagnostics: 추론 wall/CPU, QUIC packetization/crypto/lock/socket 시간, per-core/task runtime.

원본 데이터를 많이 버려 RAM만 안정화한 것을 QoS 향상이라고 하지 않는다. 동일 입력과 네트워크 조건에서 단순 latest-only보다 중요한 임무를 더 많이 완수해야 한다. 아직 GT/필수 deadline이 고정되지 않았으므로 임무 성공률의 실측 결과는 없다.

## 기존 자료의 위치

[capture10](../capture10/RESULTS.md), [weak-link](../handover_burst/RESULTS.md), [batching](../quic_batch/RESULTS.md)은 전환 이전 baseline이다. 서로 다른 FOMO/ESPDet 빌드 결과도 구분한다. 이전 MoQ-style timeout 설명은 MOQT wire interoperability 증거가 아니다.

## Concurrent capture and AI track

The opt-in two-track stage is described in [CONCURRENT.md](CONCURRENT.md). It uses
ESPDet cat224 and separates camera acquisition from MoQ submission. Failed RGB
allocation runs are explicitly excluded from successful AI workload results.

## 송신 루프·슬롯 순서 개선

[후속 실측](PUMP_RESULTS.md): 완성 영상15→39장, 오래된 슬롯이 새 프레임에 밀리던 최대25.13초 점유를3.24초로 줄였다. AI 추론 중앙값은2.23→2.50초로 증가했으며, 수집 최대간격260ms도 남아 있어 전체 QoS 개선으로 단정하지 않는다.

## 영상 동시 전송 1개 vs 2개

[실보드 비교](SLOT_COMPARISON.md): 슬롯 점유 p50은1개1.73초 / 2개2.92초, 완성 영상은34/601장 / 41/601장이다. AI 추론은약2.5초로 큰 개선이 없었다. 최신 상태 전달을 위한 다음 고정 기준선은1개로 두고, 2개는 전송량 비교군으로 유지한다.

## 다중 알고리즘 비교 설계

[비교 프로토콜](ALGORITHM_COMPARISON.md)은 admission과 전송 선택을 분리한다. A0 bounded FIFO와 S1 latest의 admission 비교, S0–S5 FIFO/AI 우선/DRR/EDF/중요도-EDF/draft-21 priority의 선택 비교를 수행하도록 설계했다. `algorithm_matrix.py`는7설정×3네트워크×3반복의63개 **미실행 계획**을 생성한다. 기존 다섯 선택 정책과 AI 이벤트 FIFO로 수행한 6설정 실보드 진단은 [구현·진단 결과](POLICY_RESULTS.md)에 보존했다. 새 S5는 host 검증, 전체 펌웨어 빌드와 one-slot 실보드 pilot을 통과했지만 반복 mission matrix에는 아직 포함하지 않았다. 문헌별 적용 범위와 S1/S5 pilot은 [문헌 기준선 지도](LITERATURE_BASELINES.md)를 따른다. 기존 진단은 GT를 포함한63개 임무 평가를 대체하지 않는다.

[현재 비교 상태](COMPARISON_STATUS.md)는 A0/S0–S5와 T0 원격 실기 결과를 구분한다.
T0의 2초/UXGA 설정은 128 kbit/s에서 preview를 모두 잃어 탈락했다.
후속 [v17 비교 규약](../../papers/mission_aiot_moq/EXPERIMENT_CONTRACT.md)은 동일 입력과
고정 high/medium/low, timeout, 적응형 representation의 무작위 원격 비교를 정의한다.
[자동 생성 결과](results/ladder-v17-screen/RESULTS.md)는 진행 중인 run을 완료 결과로 세지 않는다.
이는 사전 인코딩된 정지 영상 replay의 전송 평가이며, live encoding이나 GT 미션 성공률 결과가 아니다.

## 반복·대역폭 제한 비교

[후속 결과](REPEAT_RESULTS.md): 6설정 재시험과512kbit/s·추가50ms의3설정 비교를 완료했다. 제한 링크에서 AI 우선은 FIFO와 같은 영상24장을 전달하면서 AI 입력→ACK p95를4.66→3.54초로 줄였다. DRR의 추가 우위는 확인되지 않았다. 입력 로그 교차 검증과 이전 A0 분모 오류 정정도 포함한다. 실제 카메라10Hz·GT 임무 성공률·핸드오버 검증은 아니다.

## 연구 실험 로드맵

[RESEARCH_ROADMAP.md](RESEARCH_ROADMAP.md)에 현재 증거, 단계별 실험, 정적 pacing/미ACK 바이트 상한 비교, 실제 나사 GT 평가, KPI와 연구 진행·중단 기준을 정리했다. 즉시 단계는 계측 안정화와 대기 원인 분리이며, 동적 제어는 최선의 고정 설정으로 해결되지 않을 때 진행한다. 새 계획의 실험들은 아직 미실행이다.

## 실제 카메라 원격 성능검증

[2026-09-23 결과](REMOTE_PERFORMANCE.md): UXGA+FOMO의 native MOQT-21 두 트랙을
무제한/512/256/128 kbit/s에서 측정했다. 128 kbit/s에서도 AI 객체 12건은 모두
완성됐지만 preview는 10장으로 감소했다. 동일 조건의 S0/S1 직접 비교에서 단순
AI-first는 source→ACK를 개선하지 못했다. 이미 QUIC에 제출된 preview bytes는 이후
우선순위가 선점하지 못하므로 다음 비교는 unacked-byte cap과 stale stream 취소를
포함한다. 아직 GT mission success나 통계적 우위 결과는 아니다.

## P0 계측 원장

[AUDIT_IMPLEMENTATION.md](AUDIT_IMPLEMENTATION.md): opt-in384KiB 원장에 측정 로그를 저장하고 종료 후 CRC·ACK·재전송으로 회수한다. 실보드에서600개 입력과24개 실제 AI 결과의 최종 상태를 대사했으며, ACK 누락 주입 후 재전송도 검증했다. 시계 동기화와 전체 P0 완료를 뜻하지 않는다.

## 헬퍼 ESP32 (AIOT_MOQT_HELPER)

두 번째 ESP32가 릴레이에서 카메라 프레임 트랙 `aiot/esp32 offload/jpeg`를 MoQ 구독자로 받아 카메라와 같은 검출기(model-f1, ROI x8)를 돌린다. 카메라·발행 없음, 최신 프레임 정책(추론 중 새 프레임은 대기 1개만, 덮인 것은 dropped). 코드: `src/main/app_moq_helper.inc`, `src/components/transport/moqt_helper_sub.inc`(구독·subgroup 파서), 옵션은 루트 `CMakeLists.txt`(`AIOT_MOQT_HELPER`, 기본 OFF; `_HOST`/`_PORT`/`_TRACK`/`_REPORT_HOST`/`_REPORT_PORT`).

- 빌드: `./helper_bench.sh build` → `vendor/build-draft21-s3-helper` (릴레이 192.168.0.38:14600, 보고 UDP 192.168.0.38:5320, netlog 끔). 벤치용 `./helper_bench.sh build-bench` → `helper-bench` (15600 / 15320).
- 재플래시(3f38, /dev/ttyACM0, 시리얼 모니터를 먼저 닫을 것): `./helper_bench.sh flash helper`
- 벤치: `./helper_bench.sh bench OUT 120 [--camera-frames] [--period 0.3]` — 릴레이(15468/15600, `--offload`) + 트레이스 프레임 발행 모델 카메라 + 시리얼 로그 + `helper_bench.py analyze`.
- MoQ 결과 트랙: 헬퍼는 같은 세션에서 `aiot/esp32 helper/events`(`AIOT_MOQT_HELPER_EVENTS_TRACK`)의 발행자이기도 하다. 추론마다 객체 1개(Group = 카메라 프레임 시퀀스, 속성 0x78 = capture_ms, 페이로드 = 아래 UDP와 같은 JSON, 발행자 우선순위 64, 전달 타임아웃 2 s, 보존 없음). SETUP Implementation이 `aiot-helper/1`이라 릴레이(`moq_relay --helper-events`)가 그 세션에 SUBSCRIBE helper/events를 보내고, 받은 객체를 릴레이 트랙 helper/events(트랙 인덱스 4, **alias 5**, 타임아웃 없음)로 저장·전달한다(relay.jsonl `up_object`에 `"source":"helper","sess":N`). 구독: `SUBSCRIBE ns=("aiot","esp32") name=b"helper/events" params=[(0x20 SUBSCRIBER_PRIORITY, 0..64)]`, 필터 없음 — `helper_events_sub.py --connect HOST:14600` 또는 `receiver_draft21.py --connect HOST:14600 --helper-events`(events.jsonl의 `"helper"` 필드). 시험용 헬퍼 대역: `helper_bench.py standin --down 127.0.0.1:15600`.
- 보고: 추론마다 UDP 데이터그램 1개 = JSON `{"group","capture_ms"(속성 0x78),"recv_us","start_us","end_us","dets":[[x,y],..],"n","ran","backend","bytes","first_us","has_capture","dropped","inferred"}` (*_us는 헬퍼 시계). 콘솔에는 `[HELPER_DET] ` 접두. 연결 실패·릴레이 재시작 시 2 s 후 재연결·재구독(유휴 5 s로 끊김 감지).
- 결과(2026-10-05, `results/helper-bench-20261005/`): 1 fps에서 117/117 추론, 손실 0, 추론 0.48 s(p50), 수신(전송) 0.20 s, 발행→보고 도착 0.77 s(p50)/0.85 s(p95); 카메라 자신의 검출(MOQ_TRACK)과 117/117 일치. 0.3 s 주기 과부하에서 1.7 추론/s, 60 중 29 대체(drop). MoQ 결과 트랙(`results/helper-bench-20261005/events/`): 117/117 객체가 릴레이를 거쳐 구독자에 도착, 페이로드는 UDP와 동일, 발행→MoQ 도착 0.76 s(p50)/0.84 s(p95), UDP보다 0.8 ms(p50) 늦음. 프레임 상한 320 KB(`AIOT_MOQT_HELPER_MAX_KB`/`_FRAME_KB`): 231–291 KB 프레임은 추론(최대 1.0 s), 381 KB 이상은 overflow로 계수(`big/`); receiver_draft21 `--helper-events`의 events.jsonl에 `"helper"` 기록 확인.
