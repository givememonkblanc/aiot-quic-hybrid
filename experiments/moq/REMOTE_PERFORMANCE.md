# 원격 ESP32 MoQ 성능 검증 — 2026-09-23

## 범위와 판정

실제 ESP32-S3에서 native MOQT draft-21 publisher, `preview/jpeg`와
`ai/events` 두 트랙, UXGA(1600×1200) JPEG 및 TFLM/FOMO를 함께 실행했다.
수신기는 SETUP/두 SUBSCRIBE, 완전한 객체, JPEG 해시·decode, 트랙 종료를
검증했다. 이 결과는 **MoQ 전송 경로의 기능 및 혼잡 반응 pilot**이다. 나사
GT가 없으므로 detection 정확도나 mission success를 뜻하지 않는다.

## 대역폭 sweep

모든 제한은 해당 ESP32→수신기 UDP flow에만 적용했고 50 ms 지연, 64 packet
queue, 설정상 random loss 0%였다. 각 조건은 1회이며 입력 장면과 JPEG 크기는
고정 trace가 아니다.

| 조건 | Preview 완성 | Preview payload goodput | Preview 간격 p95 | AI 완성 | Capture→AI 관측 p95 |
|---|---:|---:|---:|---:|---:|
| 무제한 live2 | 32 | 359.4 kbit/s | 2.190 s | 11 | 7.787 s |
| 무제한 repeat | 37 | 372.6 kbit/s | 1.826 s | 11 | 7.561 s |
| 512 kbit/s | 36 | 362.3 kbit/s | 1.850 s | 11 | 7.431 s |
| 256 kbit/s | 20 | 201.1 kbit/s | 3.011 s | 12 | 7.553 s |
| 128 kbit/s | 10 | 105.3 kbit/s | 6.189 s | 12 | 6.547 s |

512 kbit/s는 장치의 실제 약 0.36–0.37 Mbit/s payload rate보다 높아서 병목
조건이 아니다. 256/128 kbit/s에서 preview 완성률과 갱신주기가 링크에 맞춰
감소했고, 같은 실행에서 생성된 AI 객체는 모두 수신됐다. qdisc drop은 0이라
이 결과는 loss recovery보다 rate backpressure 특성이다.

## 동일 조건 제출 제어 × selector 비교

정책과 입력 선택을 분리했다. 두 v11 펌웨어는 실제 카메라, 동일 모델,
한 preview slot, latest pending admission, preview 2 s/AI 10 s source-relative
threshold를 사용한다. 유일한 정책 차이는 S0 oldest-admission-first와 S1
AI-first이다. 이전 AI 객체의 source→QUIC ACK를 다음 AI 객체에 실어 ESP32의
동일 clock domain에서 원격 회수했다.

| 128 kbit/s, 각 1회 | S0 무제한 | S1 무제한 | S0 cap 32 KiB | S1 cap 32 KiB |
|---|---:|---:|---:|---:|
| Preview 완성 | 10 | 10 | 10 | 10 |
| Preview payload goodput | 102.6 kbit/s | 103.1 kbit/s | 105.8 kbit/s | 101.6 kbit/s |
| AI 완성 | 12 | 12 | 12 | 12 |
| AI source→ACK 표본 | 11 | 11 | 11 | 11 |
| AI source→ACK p50 | 7.927 s | 8.976 s | **7.819 s** | 7.824 s |
| AI source→ACK p95 | 10.493 s | 10.590 s | 7.998 s | **7.997 s** |
| Capture→AI 관측 p95 | 6.010 s | 6.137 s | 6.207 s | 6.052 s |
| Shaper packet drop | 38/916 | 39/929 | **0/953** | **0/913** |

무제한 단일 반복에서 S1 우위는 없었다. p50은 오히려 S1이 1.049 s(13.2%)
길고 p95 차이는 0.096 s(0.9%)로 비슷했다. 32 KiB cap에서도 S0/S1 p50과
p95는 사실상 같았다. 현재 workload에서 AI-first selector의 추가 이득은
관찰되지 않았으며 표본 수가 작아 어느 selector의 우위도 주장하지 않는다.

반면 cap 자체는 두 selector 모두에서 tail을 줄였다. S0 p95는
10.493→7.998 s(-23.8%), S1은 10.590→7.997 s(-24.5%)였다. 완성 객체 수를
유지하면서 shaper drop도 38/39건에서 0건으로 줄었다. 이는 application의
outstanding-byte bound가 QUIC/Wi-Fi로 향하는 burst를 완화했다는 pilot evidence다.
입력 JPEG 크기가 run마다 달라 반복 실험 전에는 인과효과나 일반화된 개선율로
확정하지 않는다.

원인은 현재 selector가 QUIC에 **아직 제출하지 않은** stream slice만 선택할
수 있기 때문이다. 큰 preview가 생성 직후 ngtcp2 송신 버퍼에 많이 받아들여지면,
이후 완성된 AI 결과는 이미 제출된 preview bytes를 선점하지 못한다. 따라서
track priority만 바꾼 실험은 충분한 preemption mechanism이 아니다.

## 제외한 시험과 계측 한계

`moqt-ota-fomo-aifirst-v9-shaped128-20260923-r1`은 정책 플래그가 replay 입력도
강제했지만 OTA 이미지에는 replay JPEG가 빠져 객체가 0개였다. transport 실패가
아니며 성능 표에서 제외했다. 이후 `AIOT_MOQT_REPLAY_INPUT`을 정책과 분리했다.

수신기는 두 트랙의 `finished`와 오류 0건을 확인했다. shaped run에서 host가
scheduled CONNECTION_CLOSE를 보기 전에 harness가 종료돼 `closed_cleanly=false`인
경우가 있다. 이는 객체/트랙 완성 실패로 세지 않되 종료 관측 절차는 보완해야 한다.

현재 FOMO 결과는 현 장면의 오탐을 포함하고 GT가 없다. Capture→AI 관측은 inference
경로 지연이며 서버 전달 지연이 아니다. source→ACK는 receiver completion보다 늦을
수 있는 상한 proxy다. 각 조건 1회이므로 신뢰구간이나 유의성 주장은 하지 않는다.

## 논문 성능검증 로드맵

1. **제출 제어 반복/sweep:** 이번 0/32 KiB pilot을 고정 trace에서 반복하고
   16/64 KiB도 측정한다. 현 결과는 cap 효과는 보이지만 selector 추가효과는 없다.
2. **stale object lifecycle:** deadline이 지난 active preview stream에
   `RESET_STREAM`/`STOP_SENDING`을 적용하고, 취소 byte·재전송 byte·RAM 회수를 기록한다.
3. **재현성:** 고정 JPEG/AI-event trace로 network condition별 최소 5회, seeded random
   order로 실행한다. primary는 실제 입력 전체 분모의 deadline success와
   source→ACK p95/p99, secondary는 preview freshness/goodput와 resource다.
4. **비교군:** 동일 JPEG·암호화·링크에서 raw QUIC single queue, MoQ S0,
   MoQ S1, capped/cancellable S1을 비교한다. framing과 reliability 차이를 통제한다.
5. **mission 단계:** 턴테이블 나사 GT로 event와 필수 evidence를 연결하고
   `event+evidence`가 deadline 안에 모두 완성된 비율을 primary KPI로 둔다.
6. **mobility 단계:** 재현 가능한 outage trace 후 실제 약전계/AP 전환을 수행한다.
   disconnect 동안 버린 bytes만으로 성공을 주장하지 않고 mission completion을 비교한다.

다음 구현 판단 기준은 명확하다. cap/cancellation을 넣고도 S1이 최선의 단순 고정
정책보다 mission deadline success를 높이지 못하면 새 scheduler 주장은 중단한다.
반대로 AI/evidence deadline을 올리면서 preview freshness와 자원 한도를 지키면 그
trade-off가 Mission-Based AIoT Media over QUIC의 핵심 결과가 된다.

기계 판독 요약은 `results/remote-performance-20260923.json`, 각 run의 원자료는
해당 디렉터리의 `events.jsonl`, `remote-summary.json`, `manifest.json`,
`shaper-end.txt`에 있다. 모든 원격 시험 후 운영 이미지
`0ccf545c...b8e9` 복구를 확인했고 flow shaper도 제거했다.
