# Mission-Based AIoT Media over QUIC 연구 실험 로드맵

작성: 2026-09-22. 상태: 실험 계획이며 아래 신규 실험은 미실행.
근거: [반복·제한 링크 실측](REPEAT_RESULTS.md), [정책 구현](POLICY_RESULTS.md).
로드맵 작성 당시에는 장치 플래시나 네트워크 설정을 변경하지 않았다. 이후 구현·실험 이력은 아래 진행 링크에서 구분한다.

구현 진행: [P0 원장·정산 1차 검증](AUDIT_IMPLEMENTATION.md)을 완료했다.
CRC·재전송과 전체 입력 대사를 실보드에서 확인했으며, 시계 오차 계측 및
계측 영향의 반복 검증은 남아 있다. 아래 나머지 실험들은 여전히 계획이다.

## 1. 연구 질문과 범위

**동일한 자원 예산과 입력·네트워크 조건에서, ESP32 비전 노드가 10Hz 수집과
필수 영상 서비스 수준을 유지하면서, 모든 실제 나사 통과 mission 중 중요한 AI
이벤트와 해당 증거를 기한 안에 정확히 전달하는 비율을 MoQ 자원 제어가 얼마나
향상시키는가?**

첫 대상은 로컬 객체 탐지와 원격 시각 확인을 함께 수행하는 constrained visual
AIoT 노드다. Preview는 작업자가 장면을 확인하는 서비스, evidence는 특정 이벤트에
연결된 증거다. 해당 응용에 불필요한 영상을 부하 발생 목적으로 추가하지 않는다.
나사 턴테이블은 반복 가능한 이벤트 평가 장치이며, 객체 개수를 CNN 부하로 간주하지
않는다. 10Hz는 실제 카메라 수집 요구이며 추론/송신 10FPS와 다르다.

주요 연구 후보는 **전송 순서 + preview 주입량 + 이미 전송 계층에 넘긴 바이트**의
공동 제어다. 새 동적 알고리즘의 필요성, MoQ 고유 기여, 연구 신규성은 아직 미확정이다.
VLM/split, MP-QUIC, 모델 선택, 에너지는 이 가설 검증 이후의 확장으로 둔다.

## 2. 현재 증거와 아직 말할 수 없는 것

| 구분 | 현재 상태 | 의미 |
|---|---|---|
| 정책 비교 | FIFO, AI 우선, DRR, EDF, 중요도 EDF, draft-21 priority + FIFO 보관 비교 구현 | S5 one-slot 실보드 pilot 완료; 반복 mission sweep은 미실행 |
| 제한 링크 | 512kbit/s + 추가 단방향 50ms에서 FIFO→AI 우선: AI 입력→ACK p95 4.66→3.54s, JPEG 둘 다 24장 | 우선순위 효과의 pilot 증거 |
| 남은 대기 | AI 우선 admission→ACK p95 약 0.93s | 전송 계층 아래의 대기 후보; 원인 확정 아님 |
| 링크 큐 | IFB backlog 약 50–57KB 관찰 | 512kbit/s에서 약 0.8–0.9s에 해당하나 객체별 인과 계측 필요 |
| 단순 정책 | DRR·중요도 EDF의 AI 우선 대비 뚜렷한 우위 없음 | AI 우선을 강한 기준선으로 유지 |
| 입력/모델 | 단일 JPEG 반복 + 실제 cat224 추론, 대부분 음성 결과 | 나사 검출 품질·GT 임무 성공률 근거 없음 |
| 시각/계측 | 입력→ACK 대리지표, USB 로그 누락과 원본 A0 분모 오류 발견 | 신뢰할 수 있는 원장과 수신 시각 계측이 우선 |
| 10Hz | 현재 비교는 메모리 재생 | 실제 카메라 10Hz를 증명하지 못함 |
| 무선 이동 | host ingress shaping만 수행 | 약전계·단절 복구·AP 핸드오버 증명 아님 |

## 3. 단계와 진행 기준

| 단계 | 핵심 질문 | 작업/산출물 | 다음 단계로 가는 기준 |
|---|---|---|---|
| P0 계측 안정화 | 결과를 신뢰할 수 있는가? | bounded event ledger, 종료 총계/CRC, 실패 원인, 시계 오차, 입력/네트워크 manifest | 모든 이벤트의 처리 결과가 대사되고 계측 비용을 보고할 수 있음 |
| P1 대기 원인 분리 | AI 우선 뒤 0.93s는 어디에서 생기는가? | 고정 메시지 trace, 실제 추론 실험 분리, QUIC/소켓/host 큐 계측, preview-off 기준 | 부하를 바꿨을 때 특정 대기 성분이 함께 변하는 증거 |
| P2 정적 제어 비교 | 고정 pacing/바이트 상한으로 충분한가? | AI 우선·고정 pacing·고정 outstanding cap·결합 비교, 파라미터 고정 | 가장 좋은 고정 설정으로도 요구를 충족하지 못하는 구간이 남음 |
| P3 실제 임무 평가 | 나사 이벤트를 정확히 제때 전달하는가? | 데이터/GT, 모델 고정, 실제 10Hz, 이벤트 및 evidence 대응 | GT 전체 분모의 기한 내 성공률과 품질/preview 제약을 함께 계산 가능 |
| P4 동적 제어 검증 | 환경 변화에 고정 설정보다 유리한가? | holdout 가변 대역폭 trace, 최소 동적 정책, ablation | 조정 비용까지 포함해 최선의 고정 기준보다 실질적 개선 |
| P5 실제 무선/복구 | 통제 실험이 실제 환경에서도 유지되는가? | 약전계, 실제 AP 전환, reconnect/cancel 회계, 장시간 실행 | 반복 가능한 임무 성능·복구 결과와 범위 제한 제시 |

P0→P1→P2가 즉시 실행 순서다. P3의 촬영/GT 준비는 P0~P2 동안 병행할 수 있다.
P2에서 정적 설정이 충분하면 P4를 만들지 않는다. P5의 단절 시험은 정상적인 세션
복구와 객체 취소/정산 구현을 검증한 뒤 진행한다.

## 4. P0: 계측을 먼저 고정

- 이벤트마다 source/frame ID, GT event ID(있는 경우), source·AI start·AI end·admission·
  QUIC accepted·ACK·server complete 시각과 최종 상태를 기록한다.
- 타이밍 구간에는 고정 크기 레코드를 bounded 원장에 저장하고 이후 내보낸다.
  원장 크기/CPU 비용을 모든 정책에 동일하게 적용한다. overflow는 조용히 덮지 않고
  측정 불완전으로 표시한다. 장시간 시험에는 배출 비용까지 포함한 대안을 검증한다.
- 종료 총계와 고유 ID, 수신 payload hash, 중복/거절/만료/취소/미수신을 대사한다.
  미전달을 포함한 실험 종료 cohort와 drain 기간을 사전에 정의한다.
- 통신/모델 실패는 실험 결과로 남기고, 계측 무결성 실패는 별도 invalid로 남긴다.
  재시험 규칙을 미리 정하며 성공할 때까지 반복하여 실패를 숨기지 않는다.
- 송수신 clock offset/drift/불확실성을 측정한다. 비대칭 링크에서 RTT/2를 정확한
  offset으로 가정하지 않는다. 수신 기한 판단은 오차구간을 포함해 성공/실패/불확실로
  구분한다. 동기화 전에는 source→ACK를 대리지표로만 사용한다.
- 계측 on/off 또는 최소 계측과 비교해 latency, capture, RAM에 미치는 비용을 확인한다.
  신규 계측 펌웨어 결과를 이전 v6 값과 동일 조건으로 간주하지 않는다.

## 5. P1: transport 진단과 AI 임무를 분리

두 종류의 입력을 별도로 사용한다.

1. **고정 통신 trace:** JPEG 바이트·크기·발행시각과 AI 메시지 바이트·발행시각을
   파일로 고정한다. 동시 AI 계산을 유지하면 그 실제 결과를 이 trace 메시지와 분리해
   기록한다. 이 실험에서 메시지는 합성/기록 재생이며 검출 성공을 뜻하지 않는다.
2. **실제 AI trace:** 같은 영상/GT를 입력하고 실제 추론이 낸 결과를 전송한다.
   정책에 따라 AI 완료 수가 달라지는 것도 시스템 결과로 계산한다.

우선 preview-off + AI metadata, preview-on + AI 우선으로 분리한다. 실제 camera
DMA/background 작업의 유무도 명시하고 모든 행에서 같게 유지한다.

| 계층 | 필요한 계측 | 주의 |
|---|---|---|
| Application | 대기 객체/바이트, admission age, 미제출 바이트, 만료 | latest 폐기와 실패를 구분 |
| QUIC | stream별 accepted/ACKed 고유 바이트, bytes-in-flight, cwnd, RTT, flow-blocked 시간, 재전송 | app 고유 바이트와 wire bytes-in-flight는 다른 양 |
| Crypto/CPU | 암호화·packet 생성 실행시간, core/task runtime, AI wall/CPU | wall waiting을 전부 scheduler 때문이라고 단정하지 않음 |
| Socket/Wi-Fi | submit 시간, 오류/backpressure, 가능 범위의 송신 완료/재전송 | API가 성공했다고 무선 송신 완료로 해석하지 않음 |
| Host impairment | backlog 시계열, drop, 실제 통과 bytes, shaping 전후 시각(가능 시) | host IFB 큐를 ESP32 내부 큐라고 부르지 않음 |

대역폭 제한만, 고정 대역폭+추가 지연, 이후 손실을 순차 적용해 변수를 분리한다.
512kbit/s에서 145989B 이미지의 직렬화만 약 2.28s이므로 2s 전체 이미지 기한은
불가능한 영역으로 표시한다. 더 작은 evidence/ROI는 별도 품질/증거 평가 변수다.

## 6. P2: 최소 정적 비교와 실험 규모

공통으로 latest preview 보관, AI 우선, 동일 모델/코어/우선순위/메모리 상한/1KiB
선택 단위를 사용한다. 영상 완성률을 희생해 AI만 빨라지는 해를 막기 위해 응용의
최소 preview 갱신률 또는 최대 갱신 공백을 먼저 고정한다.

| ID | 정책 | 검증하려는 효과 |
|---|---|---|
| B0 | AI 우선, 현재 주입 방식 | 반드시 이겨야 할 단순 기준선 |
| B1 | B0 + 고정 preview pacing | 제출 속도/버스트 제한 |
| B2 | B0 + 고정 preview outstanding-byte 상한 | 전송 계층에 미리 넘긴 데이터 제한 |
| B3 | B0 + pacing + byte 상한 | 두 기법의 결합 |

AI-only는 별도 진단 하한이며 preview 요구가 있는 임무의 경쟁 해법으로 세지 않는다.
기존 FIFO/DRR은 대표 조건의 anchor로 유지한다. 모든 정책을 모든 조건에 곱하지 않는다.

- 초기 상한 후보: 4/16/64KiB의 **고유 preview accepted-but-not-ACKed bytes**.
  QUIC stream ACK callback에서 중복 ACK/범위를 정확히 처리한다. 객체 전체를
  보존해야 하는 RAM 비용과 이 바이트 상한을 혼동하지 않는다.
- pacing 후보는 별도 calibration에서 preview 예산 25/50/75%를 탐색한다. 평가 trace의
  미래 대역폭을 고정 설정에 알려주지 않는다. tuning 후 절대 rate/burst를 고정한다.
- pacing/cap은 preview 신규 제출에 적용한다. QUIC congestion control, 재전송,
  제어 메시지/ACK 진행을 임의로 우회하거나 막지 않는다. chunk를 별도 객체로 만들지 않는다.
- cap으로 FIN/ACK 순환이 막히는 경우, 큰 객체의 점진 제출과 ACK 해제부터 검증한다.

첫 확인 matrix는 **4정책 × 3네트워크 × 5독립 반복 = 60 runs**로 계획한다.
네트워크는 N0 무제한, N1 512kbit/s rate-only, N2 같은 rate+추가50ms를 사용한다.
이는 기존 미실행63-cell mission matrix와 다른 인과 진단이며 중복 실행 계획이 아니다.
튜닝 sweep과 preview-off 점검은 60 runs에 포함하지 않는다.

초기 run 시간 예산은 warm-up 30s + 측정 180s + 최대 drain 30s다. 적용 가능한
연결/객체 timeout을 확인한 후 고정하며, drain 종료 시 미완료도 분모에 남긴다.
현재 60s/10s 펌웨어를 바꾸고 lifecycle을 먼저 검증해야 한다. 60 runs의 warm-up·측정·drain 합계
예산은 약4시간이며 flash/복구·실패 분석 시간은 별도다. 5반복은 시작 규모로서
p99/희귀 실패율의 충분한 표본을 보장하지 않는다.

네트워크/반복 block 안에서 정책 순서를 무작위화한다. seed와 trace를 고정하고
버전/바이너리/데이터 hash를 저장한다. 통계 단위는 run/trace block이며 프레임을
독립 반복으로 취급하지 않는다. paired 차이와 run 단위 불확실성을 보고하고,
별도 pilot 분산으로 필요한 반복 수와 실질적 효과 기준을 평가 전에 정한다.

## 7. P3: 턴테이블·나사 임무와 KPI

먼저 mission contract 한 장을 확정한다: 무엇을 탐지하는지, 이벤트 시작/종료,
서버가 무엇을 언제 알아야 하는지, 증거 이미지가 필수인지, 필요한 preview 수준.
현재 cat224는 나사 모델이 아니므로 적합한 경량 탐지기를 준비하고 모델 hash를 고정한다.
FOMO/ESPDet 모델 변경 효과와 통신 정책 효과를 한 표에서 섞지 않는다.

- 데이터: 나사 종류/방향/개수/반사/배경/조명, 음성 장면, 부분 가림을 수집한다.
  학습/검증/시험은 촬영 세션·회전·가능하면 물체 개체로 나눠 인접 frame 누출을 막는다.
- GT: 각 회전의 객체 출현·퇴장 시각, class, 위치, event ID를 annotation한다.
  필요 시 광학 기준 마커/별도 기준 카메라/센서로 시간축을 검증한다. 턴테이블 속도만으로
  모든 반복이 동일하다고 가정하지 않는다.
- 실제 카메라 시험: 평균 ≥10Hz와 별도로 p95/p99/max capture gap, 100ms 초과 비율,
  누락 횟수를 보고한다. **모든 간격 ≤100ms**가 필요한지는 mission contract에서
  별도 확정한다. 평균 10Hz로 그 요구를 통과했다고 하지 않는다.
- 가시시간과 AI 입력 선택 주기/위상을 함께 sweep한다. 추론이 오래 걸려도 객체가
  들어 있는 frame을 잡았다면 나중에 탐지될 수 있으므로 `visible time < inference
  latency`만으로 미검출을 단정하지 않는다. 늦은 탐지와 샘플링 누락을 나눈다.
- evidence를 요구하면 event ID에 맞는 원본/crop을 연결하고 완성/유효성/기한을 검사한다.
  일반 preview의 최신 이미지를 해당 이벤트의 증거로 대신하지 않는다.

| 층 | 핵심 지표/분모 |
|---|---|
| Mission primary | GT 전체 중요 이벤트 중 올바른 결과와 필수 evidence가 기한 내 수신된 비율 |
| AI quality | 이벤트 precision/recall/F1, 오탐; mAP는 모델 출력·GT가 지원할 때만 |
| AI runtime | inference wall/CPU time, 실제 처리 FPS, 샘플링 누락 |
| Acquisition | 실제 capture Hz, gap 분포/초과 비율, capture failure |
| Communication | 실제 receive age, deadline curve, goodput, update gap, expiry/drop/cancel/reconnect |
| Resource | internal heap/PSRAM 각각 peak/min/largest block, queue bytes, core/task CPU |
| Optional energy | 계측기를 확보한 경우 완료 임무당 에너지, 측정 경계 명시 |

임무 성공률은 `성공 GT 이벤트 / 전체 관련 GT 이벤트`다. AI 결과를 내지 못한
이벤트도 분모에 남고, 동일 이벤트의 중복 알림을 여러 성공으로 세지 않는다.
GT 매칭 기준과 유예 오차를 사전에 고정한다. AI quality 허용 저하, 최소 preview
서비스, deadline의 수치도 시험 결과를 보고 유리하게 정하지 않는다.

AoI는 수신기가 아는 가장 최신 유효 상태의 source 시각과 현재 시각의 차이로,
갱신이 없는 구간을 포함해 time-average/peak를 계산한다. 개별 객체 지연 p95와
다르다. 모든 과거 이벤트 처리 의무를 freshness만으로 대체하지 않는다.

## 8. P4/P5: 조건부 확장과 연구 중단 기준

정적 tuning용 trace와 분리된 holdout trace에서 고정 저속/고속/속도변화 환경을
평가한다. 예시 가변 trace는 1Mbps→256kbps→1Mbps이며 구간 길이/시점은 사전에 고정한다.
ACK 기반 추정 rate/RTT와 이벤트 기한으로 preview 예산을 조절하는 최소 정책부터
시작한다. 미래 trace를 아는 oracle은 참조 상한일 뿐 deployable baseline이 아니다.

동적 정책은 tuning set에서 선정한 **가장 좋은 고정 설정**과 비교한다. 가능한
각 정적 설정의 heldout 결과도 보여 단지 나쁜 기본값을 이긴 결과인지 확인한다.
조건마다 유리한 정적 설정을 사후 선택한 값은 별도의 oracle로 표시한다.
추정 지연, 진동, 제어 CPU/RAM 비용, preview 품질 저하를 포함한다.

| 판정 | 후속 행동 |
|---|---|
| 정적 설정 하나로 목표 영역 전체 충족 | 동적 scheduler 가설 중단; 설정/측정 결과로 정리 |
| AI만 개선되고 preview/evidence 또는 F1 요구 위반 | QoS 개선으로 인정하지 않음 |
| FIFO만 이기고 잘 튜닝한 AI 우선+pacing/cap은 못 이김 | 새 알고리즘 기여 주장 보류 |
| 개선이 특정 IFB queue 크기에서만 발생 | host 큐 특성으로 범위 제한, 실물 무선에서 재검증 |
| 반복/holdout에서 trade-off 개선 유지 | 동적 제어/ablation 및 실제 무선 단계 진행 |

MoQ 고유 연구라고 주장하려면 동일한 객체 경계·신뢰성·암호화·우선순위를 가진
일반 QUIC STREAM 기준선과도 비교한다. 차이가 없다면 기여를 constrained visual
transport의 자원 제어로 한정한다. 현재 프로젝트 내부 draft-21 통신만으로
독립 구현 간 상호운용을 주장하지 않으며, 필요 시 고정 버전의 독립 peer 검증을 추가한다.

실제 약전계는 RSSI/PHY/retry 등을 기록한다. 단절·AP 변경은 서비스 복구시간,
연결/객체 ID, 중복/미완료, 메모리 회수, 임무 성공률을 측정한다. QUIC connection
migration과 애플리케이션 reconnect를 구분한다. 호스트 shaping 결과와 섞지 않는다.

## 9. 일정과 바로 다음 작업

아래는 장치/GT 준비가 된 경우의 작업 예산 추정이며 일정 보장은 아니다.

| 기간 | 핵심 작업 | 완료물 |
|---|---|---|
| 1주차 | P0 원장·시각·정산, mission contract 초안 | 계측 신뢰성 보고서, 요구사항 표 |
| 2주차 | P1 고정 메시지 재생과 큐 분해 | 남은 대기의 원인 그림 |
| 3주차 | P2 pacing/cap 구현·tuning·고정 비교 | 정적 정책 trade-off와 진행/중단 결정 |
| 2–4주차 병행 | 나사 촬영/GT/모델 검증 | 분리된 dataset, 고정 모델, 실제10Hz 결과 |
| 4–5주차 | P3 end-to-end mission 평가 | GT deadline success·F1·resource 결과 |
| 6–8주차 조건부 | P4 holdout/ablation, P5 실제 무선 | 동적 기여 검증 또는 정적 충분성 결론 |

즉시 착수할 backlog:
1. bounded ledger와 종료 정산부터 구현하고 이전 USB 누락을 검출/방지한다.
2. mission의 deadline·필수 evidence·preview 최소 요구를 실험 전에 확정한다.
3. 고정 AI 메시지 trace와 QUIC stream별 미ACK 바이트 계측을 추가한다.
4. AI 우선+고정 pacing, AI 우선+byte cap을 별도 구현해 원인을 분리한다.
5. 단순 정적 해법의 한계를 확인한 뒤에만 동적 정책 개발을 승인할 연구 근거를 만든다.

이 로드맵은 연구 진행 기준이다. 새로운 대규모 측정 완료나 알고리즘 우수성을
의미하지 않으며, 기존 결과의 로그 누락 정정 내역은 원본 문서에 유지한다.
