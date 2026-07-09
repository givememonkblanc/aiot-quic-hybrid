# SAR 항공 FOMO 사람검출 — 학습 절차 (SARD → Edge Impulse → 펌웨어)

`aerial_fomo_model_pipeline.md`가 **펌웨어 통합**(모델 바이트 교체·자산·op resolver)을 다룬다면,
이 문서는 그 앞단인 **데이터 준비 + Edge Impulse FOMO 학습 + int8 export**를 단계로 고정한다.
대상 데이터셋은 데이터셋 비교(🔍)의 1순위 **SARD**(야지 연출 조난자, 다양한 자세)다.

> **모든 걸 지배하는 두 가지 결정**
> 1. **학습 스케일 = 배포 GSD에 맞춘다.** 96×96 입력에서 사람이 ≈6–10px 미만이면 FOMO가 못 잡는다.
>    실증 고도(Phase 2, 30–60m)에서 사람이 프레임에서 차지하는 비율과 **같은 비율로 학습 데이터를 준비**한다.
> 2. **Grayscale 96×96 int8.** 펌웨어 `fill_input_tensor`는 luma(그레이스케일)를 모델에 넣는다.
>    RGB로 학습하면 색 정보가 낭비되므로 **그레이스케일로 학습**해 입력을 일치시킨다.

---

## Stage 1 — SARD 확보

- 출처: Sambolek & Ivasić-Kos **SARD** (Roboflow Universe 또는 저자 배포). 포맷: 이미지 + bbox(Pascal VOC XML 또는 YOLO txt).
- 하드 검증용으로 **HERIDAL** 소량도 확보(고고도·극소표적, Phase 3–4 일반화 검증).
- ⚠️ 라이선스: 연구용이 기본. **제품 실증은 상업 사용 가부 확인** 후 사용(불가 시 자체 부감 촬영으로 대체/파인튜닝).

## Stage 2 — 데이터 준비 (스케일 일치 + 필요시 타일링)

**2a. 배포 GSD 계산** (탐지 가능 최소 크기 확인)
```
GSD(cm/px) ≈ (고도[m] × 픽셀피치[µm]) / (초점거리[mm] × 10)
사람 세로 ≈ 1.7m → 전체프레임 px = 170cm / GSD
→ 96×96로 다운스케일 후 px = 전체px × (96 / 프레임세로px)
```
이 값이 **≥ ~8px면 타일링 불필요**(저고도), 미만이면 타일링 필요(고고도).

**2b-i. 저고도 매칭 (Phase 2 권장 시작)** — 사람이 충분히 큰 SARD 프레임/크롭을 골라 96×96로 리사이즈.
타일링 없이 전체 프레임 그대로 학습(추론도 전체 프레임 96×96이므로 스케일 일치).

**2b-ii. 타일링 (고고도·극소표적, Phase 3–4)** — 큰 항공 이미지를 96×96(또는 160) 타일로 분할, bbox 리매핑.
```python
# tile_aerial.py — 이미지를 겹침 타일로 자르고 bbox를 타일 좌표로 변환
import os, glob
from PIL import Image
TILE, OVERLAP = 96, 24                 # 96px 타일, 24px 겹침
def tiles(img_w, img_h):
    step = TILE - OVERLAP
    for y in range(0, max(1, img_h-OVERLAP), step):
        for x in range(0, max(1, img_w-OVERLAP), step):
            yield x, y, min(x+TILE, img_w), min(y+TILE, img_h)
# 각 타일에 대해: 겹치는 bbox를 타일 로컬좌표로 이동, 사람 포함 타일 + 음성 타일 일부 유지
# (사람 없는 타일 = 지형 텍스처 음성 샘플 → 오탐 억제에 중요)
```
> Roboflow를 쓰면 업로드 → **Tiling + 포맷 변환 + Edge Impulse export**를 GUI로 처리 가능(스크립트 불필요).

**2c. 음성 샘플.** 사람 없는 지형(수풀·바위·그림자) 타일을 충분히 포함해야 **지형 오탐**이 줄어든다.

## Stage 3 — Edge Impulse FOMO 학습

1. 새 프로젝트 → **Images → Object Detection → FOMO**.
2. 데이터 업로드(택1):
   - Roboflow → *Export → Edge Impulse*, 또는
   - EI Uploader:
     ```bash
     edge-impulse-uploader --category split --label person tiles/*.jpg
     # bbox는 bounding_boxes.labels(JSON) 동봉 또는 Studio에서 라벨링
     ```
3. **Impulse 설계**: Image 블록 = **Grayscale, 96×96**; Learning 블록 = **Object Detection (FOMO, MobileNetV2 0.35)**.
4. 학습 → 검증셋 **F1·precision/recall** 확인. 클래스 1개(`person`). 데이터 불균형 시 음성 비율 조정.
5. **Model testing**로 held-out 성능 확인(recall이 SAR의 핵심 지표).

## Stage 4 — int8 `.tflite` export

- Dashboard → **TensorFlow Lite (int8)** → **quantized 모델 `.tflite` 다운로드**(C++ 라이브러리 아님).
- [Netron](https://netron.app)으로 열어 **① 입력 텐서 scale/zero_point ② 출력 shape(예: 1×12×12×2) ③ op 목록** 기록 — 펌웨어 통합에 필요.
- FOMO 96×96 → 그리드 **12×12**(입력/8), 채널 = 배경 + 클래스.

## Stage 5 — 펌웨어 통합 (→ `aerial_fomo_model_pipeline.md` 참조)

```bash
xxd -i sar_person_fomo_int8.tflite > ai_inference_person_model.c   # 배열로 변환
```
`src/components/ai_inference/ai_inference_person_model.c` 교체 규약:
- `alignas(8)` → `__attribute__((aligned(8)))`, 심볼명 `g_person_detect_model_data`/`_len` 유지, `_len` 갱신, include 제거.

`ai_inference_model_asset_generated.h` 자산 수정:
```c
.input_width = 96U, .input_height = 96U,
.class_count = 1U,            // person
.background_index = 0U,       // FOMO 채널0 = 배경
.output_type = AI_INFERENCE_OUTPUT_FOMO,   // ← 분류에서 FOMO로 (populate_fomo_detections 자동)
.detection_threshold = 128U,  // 셀 확신도 임계 (현장서 튜닝)
.tensor_arena_size = 150U*1024U,  // 부족 로그 시 상향
.len = <새 바이트수>,
```
**op resolver**(`ai_inference_tflm.cc`의 `configure_resolver`): Netron으로 확인한 op 중 미등록분 추가.
FOMO는 보통 **PAD·ADD·LOGISTIC·RESIZE_NEAREST_NEIGHBOR**가 추가로 필요할 수 있다(현재 등록: MUL·MINIMUM·RELU·AVERAGE_POOL_2D·CONV_2D·DEPTHWISE_CONV_2D·RESHAPE·SOFTMAX·SUB·CONCATENATION).

## Stage 6 — 빌드·검증

```bash
. ~/esp-idf/export.sh && idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash
```
- 부팅 로그: `Inference backend=1 model_available=1 model_input=96x96` (모션 폴백 아님).
- 실패 신호: `AllocateTensors failed`(미등록 op → Stage5) · `Invoke failed`(양자화/op) · `arena alloc failed`(arena↑).
- **벤치**: held-out SARD 이미지를 카메라 앞 화면에 띄우거나 주입 → **FOMO centroid가 사람 위치에** 뜨는지, `inference_us`·CPU 확인.
- **임계 튜닝**: `detection_threshold`로 precision/recall 조절.
- **현장(Phase 2)**: 계획 표적(자원봉사자, GPS) → recall + centroid→지오로케이션 측위오차.

## 흔한 함정
- **스케일 불일치** — 배포 고도와 다른 GSD로 학습하면 실전서 실패(가장 흔한 실패). Stage 2a 필수.
- **RGB/Grayscale 불일치** — 반드시 그레이스케일 학습.
- **입력 양자화 관례** — 펌웨어 `fill_input_tensor`는 [0,1] 정규화 가정(EI 기본과 일치하나 Netron scale/zp로 확인).
- **출력 채널 배치**(background_index) — Netron + 벤치로 검증.
- **음성 부족** → 지형 오탐. Stage 2c.
- **라이선스** — 상업 실증이면 자체 부감 데이터 파인튜닝이 도메인·라이선스 양쪽에서 안전.

---
정리: **데이터 스케일을 배포 고도에 맞추고, 그레이스케일 96×96 int8 FOMO로 학습**해 `.tflite`를 뽑으면,
펌웨어는 `output_type=FOMO`로 자산만 바꿔 centroid 검출이 동작한다(전송·펌웨어 구조 무변경).
