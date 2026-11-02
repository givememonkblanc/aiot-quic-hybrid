# Pilot 해석 보정 및 다음 계측

2026-09-20. 현재 소스를 재확인한 결과다. 새 성능 측정은 아니다.

## 관찰과 원인 가설

PSRAM/전송 큐 증가와 AI 결과 수신 중단은 관찰됐다. 그러나 `camera FPS / AI processing rate`만으로 5.46MiB 큐 증가를 설명할 수 없다.

- `src/main/app_memory.inc`의 queue_bytes는 `route_payload_bytes`, 즉 전송 경로 payload 할당량이다.
- `src/components/sources/source_async.inc`의 AI 입력은 active 1개 + pending 최신 1개이며 pending은 overwrite한다. 무한 FIFO 추론 큐가 아니다. offer 자체에도 5Hz 상한이 있다.
- `src/main/app_mobility.inc`는 `app_stream_frame_at()` 성공 시에만 AI에 입력을 제출한다. 전송 admission이 추론 입력 선택까지 제약하는 결합이 있다.
- `src/main/app_main.c`의 영상 admission에는 RAM/예산/큐 공간 gate가 있다. mode 2는 추가 admission과 2초 chunk deadline도 사용한다. 현재 FIFO도 6MiB 상한이 있는 bounded FIFO이고 mode 전환은 순서만 바꾸는 단일 요인 비교가 아니다.
- 따라서 AI 0건의 원인은 입력 제출 감소, 연산 지연, 결과 송신/큐 지연, 연결 상태 등을 단계별로 분리해야 한다. AI가 오래된 FIFO 프레임을 계속 처리했다고 결론 내리지 않는다.

## 계측

공통 frame_id/event_id로 capture, AI offer/overwrite, inference start/end, result creation, enqueue/reject/drop, send, receive를 연결한다. 정책 drop과 네트워크 손실을 분리하고 송수신 clock mapping 및 오차를 기록한다.

- start − capture: 추론 시작 시 입력 나이.
- finish − capture: 추론 완료 시 입력 나이.
- receive − capture: 해당 결과의 E2E 지연.
- receiver AoI: Δ(t)=t−u(t), u(t)는 시각 t까지 수신된 결과 중 가장 최신 원본 capture 시각. 수신 없는 구간도 포함해 시간평균, time-weighted quantile, peak age를 산출한다. 시작 시 최신 정보가 없으면 미정 구간을 별도 보고한다.

출처: [Yates et al., Age of Information: An Introduction and Survey](https://arxiv.org/abs/2007.08564). 개별 수신 결과의 지연 분포를 시간축 AoI 분포와 혼용하지 않는다.

## 비교 설계

같은 전송 예산에서 큰 bounded FIFO / 작은 bounded FIFO / pure latest-only / 현재 AMR-inspired admission / proposed joint manager를 구분한다. latest-only의 적용 위치(AI 입력, 영상 출력, AI 결과 큐)를 명시한다. 기존 정책 1은 큐 비움 gate와 in-flight 제한을 포함하므로 새로운 joint manager나 pure latest-only로 부르지 않는다.

동일 센서 trace와 GT event, 자원 한도, network trace를 사용한다. AI offer를 영상 송신 성공에 연결한 현재 구조는 독립 AI offer 구조와 ablation한다. 기존 상태를 먼저 보존하고 개선 전후를 섞지 않는다.

DESR(D)는 전체 실제 중요 이벤트 중 D 이내 올바른 결과 수신 비율이며 오탐률도 병기한다. D=0.5/1/5/10초 곡선을 초기 characterization에 사용할 수 있으나 응용 요구 deadline과 구분한다. 현재 처리 시간이 수 초이므로 짧은 deadline에서 모든 정책이 0일 수 있다.

AI workload는 inference FPS만으로 low/medium/high를 정하지 않는다. 모델·입력 크기·1회 실행 비용·요청 빈도와 CPU/메모리 수요를 명시하고 실제 처리율을 결과로 측정한다. 고정 모델/입력에서 실행 빈도를 높이면 대개 요구 연산량은 증가한다.
