# AIoT QoS 고도화 로드맵

2026-09-22 전송 계층 방향 변경: [Media over QUIC 전환](../moq/README.md)을 우선한다. 아래 기존 QUIC 결과는 baseline으로 보존하며, MoQ의 효과로 해석하지 않는다. [첫 실물 MoQ 시험](../moq/RESULTS.md)은 JPEG 단일 트랙의 전송·정상 종료 검증이며, AI 동시 실행·10Hz 수집·임무 QoS 개선 결과는 아니다. [후속 두 트랙 통합 시험](../moq/CONCURRENT.md)은 ESPDet 결과27건 전달과 평균9.95Hz 수집을 확인했지만, 최대315ms 수집 간격과 영상15/598장 전달로 엄밀한10Hz·영상 QoS는 미달이다.

2026-09-21. 목표: **제한된 ESP32-S3 자원에서 중요한 AI 결과를 제시간에 전달하고, 영상의 신선도·연속성과 장치 안정성을 함께 유지한다.** QUIC batching은 이 목표를 위한 조정 수단이다.

## 서비스별 성공 조건

추가된 필수 조건은 **데이터 수집 10Hz 유지**다. 평균/구간별 수집률과 프레임 수집 간격을 따로 판정한다. [수집·CPU 구간별 측정](../capture10/RESULTS.md)에서 기존 FOMO는 목표30 설정으로 평균 약12.5Hz를 유지했지만, 별도 ESPDet 계측 빌드는 카메라 단독에서도 100ms 초과 간격을 보였다. 먼저 framebuffer 대기와 main-loop 주기 오차를 해소하고, 그 위에서 AI·전송 QoS를 평가한다. 평균10Hz를 100ms deadline 보장으로 표현하지 않는다.

| 서비스 | 우선 평가할 QoS | 조정 원칙 |
|---|---|---|
| AI 이벤트 metadata | 올바른 이벤트의 deadline 내 수신율, 누락·중복, p95/p99 | 중요 이벤트를 먼저 처리하고 전송; event ID로 중복 제거 |
| 증거 이미지/ROI | 해당 이벤트와 연결된 완성 이미지의 deadline 내 수신율 | 이벤트 연결을 유지하고, 크기·품질·허용 지연을 명시 |
| 미리보기 | 완성 프레임 FPS, 수신 간격/정지 시간, 정보 age, complete goodput | 오래된 프레임은 프레임 단위로 폐기; AI 서비스가 나빠지면 생성·전송 예산 축소 |
| 장치 | watchdog/reset/OOM, 내부 heap·PSRAM 여유, CPU 사용 | 안정성 위반 설정은 정상 성능 후보에서 제외 |

현재 metadata는 **MQTT QoS0**로 publish한다. 이 문서의 애플리케이션 QoS는 MQTT의 QoS0/1/2 번호와 구별한다. QUIC에 데이터를 제출했다는 사실만으로 서버 수신이나 이벤트 처리가 완료됐다고 판단하지 않는다.

평가를 하나의 임의 점수로 뭉개기보다 우선 순위를 둔다: **장치 안정성 → AI 품질 하한과 중요 이벤트 deadline → 증거 전달 → 미리보기 품질**. 증거 이미지 자체가 필수인 응용에서는 metadata와 증거를 함께 성공 조건으로 정한다.

## 0. 완료한 characterization: batching의 QoS 영향 확인

[약한 링크 인위적 재현 결과](../handover_burst/RESULTS.md): 256kbit/s·추가 지연200ms·손실3%에서 수집 약12.5Hz와 메모리 여유는 유지됐지만, 평상시 모드+quality 정책의 완성 JPEG 수신은 약3.12→0.11FPS로 하락했다. AI starvation은 이번 조건에서 관찰되지 않았다. RSSI/PHY/MAC 재전송은 재현하지 않았으므로 실제 약전계 검증과 구분한다. 다음 단계는 메모리 고갈 유도보다 중요 이벤트 deadline과 완성 이미지 전달률을 평가하고, 링크 용량에 맞춰 이미지 크기·전송률을 제어하는 것이다.

[QUIC batching 실험](../quic_batch/README.md): 동일한 실제 1024×1024 JPEG를 반복하고 1/4/8/16 KiB/frame 제출을 비교한다. 1,024-byte application fragmentation은 고정하고, QUIC packetization과 제출/connection-lock 횟수를 따로 센다.

- 실제 수신 complete goodput과 AI 지연, CPU, lock wait, admission drop, 미완성 프레임, watchdog을 함께 비교한다.
- 초기 시험의 RGB 버퍼 재할당 실패가 AI 부하를 없애는 교란을 확인해, 동일 3 MiB 재사용 버퍼를 모든 정식 조건에 적용했다.
- 프레임 단위에서 CPU1 idle watchdog 경고가 관찰됐다. 해당 실행은 즉시 미리보기를 중단하고 안정성 실패로 기록한다. 경고를 숨기거나 watchdog을 끄지 않는다.
- 결과물: 안정 조건의 QoS trade-off 표, 실패 조건, 반복 범위, 재현 가능한 raw 자료. 높은 goodput만으로 설정을 채택하지 않는다.

**측정 결과:** [최종 비교표](../quic_batch/RESULTS.md). 두 반복에서 1 KiB 대비 큰 묶음의 complete goodput은 약 32–42% 증가하고 수신 KiB당 계측 QUIC I/O CPU는 약 19–25% 감소했다. 동시에 AI 평균 지연은 약 14–23% 증가했다. 정상 sweep은 10/10 완료했지만 동일 펌웨어의 앞선 frame 실행에는 watchdog 경고 이력이 있다.

**현재 판단:** 호출 감소와 전송 효율 개선을 확인했다. AI/event SLO가 확정되지 않았으므로 전체 QoS의 “최적 설정”은 아직 선정하지 않는다. 다음 단계는 이벤트 deadline 계측과 중요 서비스 보호 예산이다.

## 1. QoS 계측을 먼저 완성

현재 로그로 즉시 계산할 수 있는 항목:

- 같은 MCU 시계의 capture→AI start, capture→AI finish age와 inference latency.
- 같은 서버 시계의 AI 결과 수신 간격, 완성 JPEG 수신 간격, 긴 서비스 공백.
- 성공한 추론의 source sequence와 서버 결과를 매칭한 전달 여부; 완성 JPEG 해시 검증.
- admission drop / 송신 실패 / deadline 폐기 / 수신 미완성 / 재연결을 서로 다른 원인으로 집계.

추가 펌웨어/수신기 계측:

1. `event_id`, `source_seq`, `capture_ts`, `infer_start`, `infer_finish`, `enqueue`, `tx_submit`, `server_receive`, `application_ack`를 연결한다.
2. MCU와 서버의 시계 오프셋·불확실성을 계측한다. 동기화 전에는 두 시계를 직접 빼서 one-way latency나 deadline 성공률을 만들지 않는다. MCU capture→ACK는 같은 시계로 측정할 수 있지만 ACK 왕복을 포함한 **확인 지연**으로 별도 명명한다.
3. 이벤트 출현/클래스/중요도 GT를 턴테이블 반복 입력과 연결한다. 현재 고정 JPEG의 AI 결과 수신은 올바른 나사 탐지 성능의 증거가 아니다.
4. deadline curve를 만든다. `DESR(D) = D 이내 올바르게 전달된 고유 중요 이벤트 / 모든 GT 중요 이벤트`. 검출·admission·전송에서 빠진 이벤트도 분모에 포함한다. 정상 상태의 추론 시간보다 작은 D가 가능한지 먼저 확인한다.

`receive−capture`는 전달된 샘플의 age다. 진짜 서버 AoI는 `Δ(t)=t−u(t)` (u: 서버가 보유한 최신 유효 상태의 원본 시각)로 계산해, 아무것도 오지 않는 정지 구간도 포함한다. 별개의 불량 이벤트를 최신 상태로 대체해 버려서는 안 된다.

**완료 기준:** 한 이벤트의 생애를 양 끝에서 추적하고, 누락과 deadline 실패를 재현 가능하게 계산할 수 있다.

## 2. 고정된 QoS 보호 장치부터 적용

- 중요 metadata/control이 preview backlog 뒤에 갇히지 않게 우선 서비스한다.
- image batch 크기와 실제 CPU burst/connection-lock hold 예산을 분리한다. 잠금 해제만으로 idle/AI 실행 기회가 확보된다고 가정하지 않는다.
- 미리보기 큐는 완성 가능한 최신 프레임 위주로 제한하고, 이미 전송 실패한 프레임의 꼬리는 즉시 폐기한다.
- RGB/tensor/제어·전송 버퍼 예산과 여유 메모리를 명시해, 조건에 따라 AI가 실패하면서 통신만 빨라지는 상황을 막는다.
- AI 입력 해상도·모델은 고정한 채 preview FPS/품질/전송 예산부터 조정한다. AI 품질을 바꿨다면 별도의 정확도 재평가가 필요하다.

**완료 기준:** 가장 좋은 고정 설정에서 AI deadline과 통신 SLO를 동시에 만족하는 운영 범위를 그린다. 이 단계에서 요구 범위가 모두 해결되면 고정 설정을 사용한다.

## 3. 필요가 입증된 구간에 QoS 피드백 제어

입력: event deadline 잔여 시간, AI age/지연, image queue age, 수신 goodput·정지, RTT/loss, 실제 CPU 실행시간, heap 여유.

조정: preview admission/FPS/JPEG 품질, batch 상한, TX CPU 예산, 증거 이미지 정책. 중요 이벤트의 의미와 필수 증거는 보존한다. QoS가 나빠지면 미리보기 예산을 줄이고, 회복이 지속될 때 단계적으로 늘리는 hysteresis를 둔다.

동적 제어 채택 기준: **같은 입력·자원·네트워크 조건에서 best static보다 중요 이벤트 DESR/서비스 공백을 개선하면서 AI 품질과 안정성을 유지하는가?** workload마다 다른 최적 설정이 나온다는 사실만으로 새 scheduler의 필요성을 주장하지 않는다.

## 4. 재현 가능한 확장 평가

1. Metadata only → event+evidence → event+evidence+preview 순서로 응용 부하를 분리한다.
2. 정상 Wi-Fi → 제한 bandwidth/지연/loss → 짧은 outage·handoff로 확장한다. 실험 흐름에만 제한을 적용하고 종료 후 제거한다.
3. 고정 JPEG trace → 변화하는 영상/턴테이블 GT trace로 확장하고 순서 균형·독립 반복을 확보한다.
4. 비교군: 현재 설정, bounded/latest-only, best static batching/priority, 제안 QoS 제어. 동일 보안·신뢰성 조건에서 필요할 때 UDP↔QUIC DATAGRAM, TCP/TLS↔QUIC STREAM을 추가한다.
5. VLM/split은 후속 축으로 유지한다. MCU ROI/feature/semantic payload가 같은 이벤트 품질·deadline에서 end-to-end QoS를 개선하는지 평가한다.

최종 KPI: F1/mAP 등 AI 품질, inference latency/FPS, 중요한 이벤트 DESR(D), sample age와 시간 기반 AoI, 서비스 공백, complete goodput, drop 원인, heap/PSRAM, watchdog/reset, 이후 실측 energy. **추론·전송·정확도·deadline·안정성의 개선을 서로 대체해서 주장하지 않는다.**

## MoQ 송신 처리·영상 슬롯 후속 측정

[실측 비교](../moq/PUMP_RESULTS.md): 송신 작업량 분할과 오래된 JPEG 우선 선택으로 슬롯 starvation을 제거했다. 영상15→39장에 비해 AI 결과27→24건, 추론 중앙값2.23→2.50초의 trade-off가 있으므로 최적 설정으로 확정하지 않는다. 다음 고정 기준선은 영상 in-flight 1개 vs 2개이며, 그 뒤 명시적인 기한과 reset 종료 회계를 적용한다.

[동시 영상 슬롯 비교 완료](../moq/SLOT_COMPARISON.md): 최신 상태 전달용 고정 기준선은1개로 선정한다. 1개/2개 실측의 슬롯 p50은1.73/2.92초, 완성 영상34/41장으로 trade-off가 있다. 입력 크기도 달라 전송량 차이를 슬롯 수의 순수 효과로 해석하지 않는다. 다음은 capture ID부터 서버 완성까지의 기한 계측이며, AI 추론약2.5초와 수집간격 초과는 별도 미해결 항목이다.

## 알고리즘 비교 확장

[MoQ 다중 정책 비교 프로토콜](../moq/ALGORITHM_COMPARISON.md)을 추가했다. 54개 계획 셀은 미실행이며, 같은 입력/메모리/네트워크/기한 아래 admission과 scheduling을 분리 비교한다. 기존 S1 계열을 기준선으로 두고 FIFO·DRR·EDF·중요도-EDF를 구현/검증한 뒤 실행한다. 현재 라이브 카메라 슬롯 비교를 알고리즘 순위나 논문 재현으로 재분류하지 않는다.
