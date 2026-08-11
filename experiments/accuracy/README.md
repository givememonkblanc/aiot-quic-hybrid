# 탐지 정확도 테스트베드

디바이스의 온-디바이스 분류기를 **호스트측 레퍼런스 검출기**와 대조한다.
참값은 사람이 손으로 라벨링하지 않고, 디바이스가 **실제로 전송한 그 프레임**에
강한 검출기(YOLO)를 돌려 만든다.

## 왜 이 방식이 성립하는가 — 조인 키

- 디바이스는 라이브뷰 프레임의 모든 청크에 **하나의 `gen_ts_ms`** 를 찍는다
  (`app_stream_frame`, esp_timer 도메인).
- 같은 캡처에서 나온 추론 결과를 AI 트랙(track 2)에 **같은 `gen_ts_ms`로** 보낸다
  (`app_send_ai_result`).
- 수신기는 재조립한 JPEG을 `frame_<gen_ts>.jpg`로, AI 오브젝트를
  `ai_recv.csv`에 기록한다.

→ **호스트와 디바이스의 시계를 맞출 필요가 없다.** `gen_ts_ms`가 정확한 조인 키다.

AI 오브젝트는 **v2 와이어 포맷**이라 `raw_score`(모델 자체 임계 적용 *전* 점수)를
싣는다. 임계로 잘린 0은 "모델이 얼마나 근접했는지"를 말해주지 않으므로 정확도
비교에는 쓸 수 없다.

## 측정하는 것 / 측정하지 않는 것

레퍼런스는 디바이스가 **전송한 픽셀 그대로**(160×120 JPEG)를 본다. 따라서 이 수치는
*동일 입력에서* int8 초소형 분류기가 강한 검출기 대비 얼마나 떨어지는가이다.
카메라 광학·해상도가 애초에 버린 정보는 측정 대상이 아니다.

## 절차

### 1. 아카이빙 켜고 백엔드 실행

```bash
export AIOT_IMAGE_DIR=$PWD/experiments/accuracy/frames
export AIOT_AI_LOG=$PWD/experiments/accuracy/ai_recv.csv
./backend/run.sh 19137
```

### 2. 양성·음성 구간이 모두 있는 세션 녹화

**한쪽 클래스만 있는 캡처로는 판별력을 측정할 수 없다.** 최소한:

- 카메라 앞에 사람이 있는 구간 1분 이상
- 방을 비운 구간 1분 이상

를 한 세션 안에 포함시킨다. 앉기/서기/걷기, 정면·측면·부분 가림처럼 실사용에서
나올 자세를 섞을수록 임계 추천값이 쓸모 있어진다.

### 3. 라벨링 → 평가

```bash
cd experiments/accuracy
python3 label_frames.py --frames frames --out out/labels.csv
python3 eval_accuracy.py --labels out/labels.csv --ai ai_recv.csv \
    --events ../../host_receiver/mqtt_events.csv --sweep-out out/sweep.csv
```

`label_frames.py`는 디코드가 깨진 프레임을 세어 건너뛴다(이미지 청크는 신뢰성 없는
데이터그램이라 청크 유실 시 JPEG이 깨진다). 조인이 inner join이라 해당 프레임의 AI
오브젝트도 함께 빠지므로, 양쪽이 항상 정확히 같은 프레임 집합 위에서 비교된다.

### 리포트가 답하는 것

| 섹션 | 질문 |
|---|---|
| `separation` (AUC) | `raw_score`가 사람 유/무를 애초에 분리하는가 |
| `operating points` | 지금 쓰는 `presence.enter`와 모델 `det_threshold`에서의 P/R/F1 |
| `best F1` | 이 캡처에서 최적 임계는 얼마인가 → `presence.enter` 근거 |
| `transitions` | 사람이 나타난 뒤 센서가 present라고 말하기까지의 지연 |

`--sweep-out`은 임계 0..255 전체의 혼동행렬을 CSV로 남긴다 (PR 곡선 작도용).

## 과거 캡처 라벨링

수신기 아카이빙이 생기기 전 피드도 되살릴 수 있다. 피드는 0x20 이상 바이트를 그대로
싣기 때문에 latin-1로 왕복하면 원본 바이트가 정확히 복원된다.

```bash
python3 feed_to_frames.py --feed ../../backend/aiot_feed.jsonl --out frames_old/
```

단 구 펌웨어는 청크마다 `gen_ts`가 달랐고 AI 트랙을 아예 보내지 않았으므로, 이렇게
복원한 프레임은 **레퍼런스 라벨링에만** 쓸 수 있고 디바이스 점수와 조인할 수 없다.

## 레퍼런스 검출기 유효성

이 카메라의 160×120 프레임에서 YOLO11n은 사람이 있는 프레임에 **최대 conf 0.824**를
준다(과거 캡처 818프레임 표본: ≥0.35가 473장, ≥0.5가 276장). 빈 방 697프레임에서는
최대 0.295로 `--ref-conf 0.35` 아래에 머문다. 즉 이 해상도에서도 참값으로 쓸 수 있다.

`--ref-conf`는 참값의 엄격도이므로, 바꾸면 리포트에 함께 적어야 한다.

## 기록해 둘 관측

빈 방 697프레임에서 디바이스 `raw_score`는 **94–117**이었다. 사람이 있을 때의
중앙값(146)과 구간이 겹치므로, 현재 `presence.enter=50`은 빈 방에서도 상시 발화한다.
정확한 권장값은 양성·음성이 모두 있는 캡처의 `best F1` 결과로 정한다.
