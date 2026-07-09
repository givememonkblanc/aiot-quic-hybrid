# 항공 FOMO 사람검출 모델 교체 파이프라인

SAR(재난 수색구조) 드론 실증을 위해, 현재 탑재된 **눈높이 사람분류 모델(Visual Wake Words)**을
**부감(top-down) 시점 FOMO 사람검출 모델**로 교체하는 절차. 전송 계층·펌웨어 구조는 건드리지 않는다 —
바뀌는 것은 **모델 바이트 + 자산 서술자(+ 필요시 op 목록·입력 해상도)** 뿐이다.

> 왜 FOMO인가: SAR은 "사람 있음/없음"이 아니라 **어디에** 있는지가 중요하다. FOMO는 그리드 centroid로
> 위치를 주므로 드론 GPS·카메라 기하와 결합해 표적을 지오로케이션할 수 있다. 코드에는 FOMO 후처리
> (`populate_fomo_detections`)가 **이미 구현**돼 있어 drop-in 이다.

---

## 0. 무엇이 바뀌고 무엇이 안 바뀌나

| 바뀜 | 안 바뀜 |
|---|---|
| 모델 바이트 (`ai_inference_person_model.c`) | 하이브리드 QUIC 전송 전 구조 |
| 자산 서술자 (`ai_inference_model_asset_generated.h`) | MQTT/datagram 스케줄러·수신기 |
| (필요시) op resolver (`ai_inference_tflm.cc`) | 프레즌스 이벤트·presence 파이프라인 |
| (필요시) 입력 해상도 (`ai_inference.c`, `ai_inference.h`) | 카메라 캡처→JPEG→디코드 골격 |

핵심 통합 지점은 **`src/components/ai_inference/`** 안에 모두 있다.

---

## Stage A — 항공 FOMO 모델 학습 (Edge Impulse)

1. **데이터셋 확보** (부감 사람, 라이선스 확인 필수)
   - **HERIDAL** — 산악 SAR 항공 사람 (연구용)
   - **SARD** — Search And Rescue Dataset (부감 사람 다양 자세)
   - **VisDrone** — 드론 보행자/사람 (도심 위주)
   - 필요시 자체 촬영 부감 프레임으로 파인튜닝 (드론 고도 30~60m 권장, 실증 장착과 동일 시점)

2. **Edge Impulse 프로젝트**
   - *Impulse*: Image → **Object Detection (FOMO)**
   - *Image*: **Grayscale**, 입력 **96×96** (또는 학습 리소스에 맞춰 64×64/120×120)
   - *Object Detection*: FOMO (MobileNetV2 0.35 등), 1 클래스 = `person`
   - 학습 → **int8 양자화** 필수 (MCU는 int8 커널)

3. **FOMO 출력 구조 이해** (통합 시 필요)
   - FOMO는 입력을 1/8로 다운샘플한 **그리드**를 출력: 96×96 → **12×12 × (클래스+1)** 채널
   - 채널 0 = 배경, 채널 1.. = 클래스. 셀 값이 클래스 확신도(양자화 int8).

4. **Export**: *Dashboard → TensorFlow Lite (int8)* → `*.tflite` 다운로드
   (또는 C++ library export 안의 `tflite-model` 바이트 사용)

---

## Stage B — `.tflite` → C 바이트 배열

```bash
xxd -i aerial_person_fomo_int8.tflite > aerial_fomo_model_data.c
# 산출 심볼 예: unsigned char aerial_person_fomo_int8_tflite[]; unsigned int ..._len;
```

현재 파일과 심볼명을 맞추면 서술자 수정이 최소화된다. `ai_inference_person_model.c`를 교체하되
아래 규약을 지킨다:

```c
/* ai_inference_person_model.c (교체본) */
__attribute__((aligned(8)))              /* TFLM GetModel은 8바이트 정렬 요구 */
const unsigned char g_person_detect_model_data[] = { 0x1c, 0x00, ... };
const int g_person_detect_model_data_len = <새 바이트 수>;
```

체크리스트:
- [ ] `alignas(8)` → `__attribute__((aligned(8)))` (C 파일이므로)
- [ ] `#include "*.h"` 제거 (컴포넌트 include 경로 밖 헤더 참조 금지)
- [ ] 심볼명 `g_person_detect_model_data` / `_len` 유지 (안 바꾸면 서술자 그대로)
- [ ] `CMakeLists.txt` SRCS에 파일 포함 (교체면 이미 포함됨)

---

## Stage C — 자산 서술자 수정

`src/components/ai_inference/include/ai_inference_model_asset_generated.h` 의
`g_ai_inference_generated_model_asset` 를 FOMO용으로 바꾼다:

```c
extern const unsigned char g_person_detect_model_data[];

static const ai_inference_model_asset_t g_ai_inference_generated_model_asset = {
    .data              = g_person_detect_model_data,
    .len               = <새 바이트 수>,        /* Stage B의 _len 리터럴 */
    .tensor_arena_size = 150U * 1024U,          /* FOMO는 보통 넉넉, 부족시 상향 */
    .input_width       = 96U,                    /* Edge Impulse 입력 폭 */
    .input_height      = 96U,
    .detection_threshold = 128U,                 /* 0~255, 그리드 셀 확신도 임계 */
    .class_count       = 1U,                     /* person */
    .background_index  = 0U,                     /* FOMO 채널0 = 배경 */
    .target_class_index= 0U,                     /* FOMO 경로에선 미사용 */
    .output_type       = AI_INFERENCE_OUTPUT_FOMO,   /* ← 분류에서 FOMO로 */
};
```

> `output_type = AI_INFERENCE_OUTPUT_FOMO` 로 바꾸면 `ai_inference_tflm.cc`의
> `populate_fomo_detections()` 가 자동으로 그리드를 순회하며 셀별 최고 채널을 임계와 비교해
> **정규화 x/y/w/h(centroid)** 를 채운다. 별도 후처리 작성 불필요.

### 서술자 필드 레퍼런스 (`ai_inference_model_data.h`)

| 필드 | 의미 | FOMO 값 |
|---|---|---|
| `data` / `len` | 모델 바이트 / 길이 | 새 배열 / 새 길이 |
| `tensor_arena_size` | TFLM 작업 메모리(PSRAM) | 부족시 로그 보고 상향 |
| `input_width/height` | 모델 입력 | Edge Impulse 설정과 일치 |
| `detection_threshold` | 셀 확신도 임계(0~255) | 현장서 튜닝 |
| `class_count` | 전경 클래스 수 | 1 (person) |
| `background_index` | 배경 채널 인덱스 | 0 |
| `output_type` | 후처리 경로 선택 | `AI_INFERENCE_OUTPUT_FOMO` |

---

## Stage D — Op resolver 정합 (필요시)

`ai_inference_tflm.cc` 의 `configure_resolver()` 에 등록된 op만 실행 가능하다. 현재 등록:

```
MUL · MINIMUM · RELU · AVERAGE_POOL_2D · CONV_2D · DEPTHWISE_CONV_2D
· RESHAPE · SOFTMAX · SUB · CONCATENATION   (MicroMutableOpResolver<24>)
```

FOMO(MobileNet 계열)는 보통 위로 충분하지만, 모델에 따라 **PAD · ADD · LOGISTIC ·
QUANTIZE · DEQUANTIZE · TRANSPOSE_CONV · RESIZE_NEAREST_NEIGHBOR** 등이 추가로 필요할 수 있다.

미등록 op가 있으면 부팅 시 로그에:
```
E ai_tflm: Resolver registration failed for op=...     (등록 실패)
E ai_tflm: AllocateTensors failed                       (미등록 op로 그래프 준비 실패)
```
가 뜬다. 해당 op를 `add_resolver_op(ctx->resolver.AddXxx(), "XXX")` 로 추가한다.
필요한 op 목록을 미리 알려면 `.tflite` 를 [Netron](https://netron.app) 으로 열어 노드 종류를 확인.
`<24>` 용량이 부족하면 템플릿 인자를 키운다.

---

## Stage E — 입력 해상도 (고고도 소형 표적 대응)

현재 추론 입력은 **40×30**(디코드 1/8 축소)으로, 고도에서 작아지는 사람에겐 부족할 수 있다.
FOMO 96×96 입력은 `fill_input_tensor`가 소스(40×30)를 업스케일해 채우므로 **동작은 하지만
해상도 정보가 없다.** 실효 해상도를 올리려면:

`src/components/ai_inference/ai_inference.c`
```c
/* 디코드 축소율 완화: 1/8 → 1/2 (또는 SCALE_0=원본) */
.out_scale = JPEG_IMAGE_SCALE_1_2,   /* line ~257 */
```

`src/components/ai_inference/include/ai_inference.h`
```c
/* 디코드 버퍼가 새 해상도를 담도록 확대 (예: 160×120) */
#define AIOT_INFERENCE_INPUT_WIDTH  160U
#define AIOT_INFERENCE_INPUT_HEIGHT 120U
```
- `g_decode_buffer` 는 `W*H*2` 로 계산되므로 자동 확대되나, **커지면 정적 DRAM 압박** →
  필요시 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 로 PSRAM 이전 고려(부팅 RAM 여유 확보).
- 카메라 캡처 프레임사이즈도 함께 올려야 실제 디테일이 생긴다
  (`src/components/sources/source_manager.c` 의 `FRAMESIZE_*`).
- 트레이드오프: 해상도↑ = 추론시간·대역·전력↑. Phase 2 현장 데이터로 고도별 최소 해상도를 결정.

---

## Stage F — 빌드 · 플래시 · 검증

```bash
. ~/esp-idf/export.sh
idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash
```

부팅 로그에서 **정상 신호**:
```
I source_manager: Inference backend=1 model_available=1 tensor_arena=... model_input=96x96 ...
```
- `backend=1` = TFLM 활성(모션 폴백 아님)
- `model_available=1` = 자산 로드됨
- `model_input=96x96` = 서술자 입력과 일치

**실패 신호**(→ 조치):
| 로그 | 원인 | 조치 |
|---|---|---|
| `Schema mismatch model=.. runtime=..` | TFLite 스키마 버전 불일치 | 최신 변환기로 재export |
| `Tensor arena allocation failed` | arena 부족/PSRAM 부족 | `tensor_arena_size` 조정 |
| `AllocateTensors failed` | 미등록 op | Stage D |
| `Invoke failed` | 런타임 op/양자화 문제 | Netron 점검·재양자화 |
| `backend=` 없음/모션만 | `model_asset_available()`=false | `data/len/arena/input` 유효성 확인 |

> 모델 로드가 실패해도 `ai_inference.c` 는 **모션 검출로 자동 폴백**하므로 기기는 죽지 않는다.
> 반드시 `backend=1` 을 눈으로 확인할 것.

**벤치 검증(비행 전)**: 부감 샘플 이미지를 카메라 앞에 두거나, 저장 JPEG를 주입해
탐지 confidence·추론시간(`inference_us`)·CPU를 확인.

---

## Stage G — 현장 지표 훅 (실증 연결)

FOMO centroid가 나오면 SAR 지표가 성립한다:
- `detection_count > 0` → presence/탐지 이벤트 (기존 파이프라인 그대로 datagram 전송)
- centroid `x/y` + 드론 GPS·고도·카메라 기하 → **표적 지오로케이션**
- 계획된 표적 GPS와 비교 → **precision/recall·측위오차**
- 전송 지표(AoI·전달률·손실)는 `analyze_sweep.py` 계열 필드 확장으로 산출

---

## 요약 체크리스트

- [ ] Stage A: HERIDAL/SARD로 Edge Impulse FOMO(int8, 96×96, grayscale) 학습·export
- [ ] Stage B: `xxd -i` → `ai_inference_person_model.c` 교체 (aligned(8), 심볼명 유지, len 갱신)
- [ ] Stage C: 서술자 `output_type=FOMO`, `input_width/height`, `len` 갱신
- [ ] Stage D: Netron으로 op 확인 → 미등록 op를 resolver에 추가
- [ ] Stage E: (고도 필요시) 디코드 스케일·버퍼·카메라 프레임사이즈 상향
- [ ] Stage F: 빌드·플래시 → 로그 `backend=1 model_input=WxH` 확인
- [ ] Stage G: centroid→지오로케이션, 전송 지표 필드 연결

> 전송·펌웨어·수신기·분석기는 그대로. **"모델만 바꾸면 임무가 바뀐다"** 를 이 문서가 절차로 고정한다.
