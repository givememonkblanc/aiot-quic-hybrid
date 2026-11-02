# MCU AIoT 디바이스의 추론–통신 공동 자원 관리

조사일: 2026-09-20. 이번 작업은 문헌 조사와 현재 소스 대조다. 새 펌웨어 배포나 실기 성능 검증 결과가 아니다. 원문/공식 저장소에서 확인한 사실, 프로젝트에 대한 관찰, 제안하는 연구 설계를 구분한다.

## 1. 연구 질문

**동일한 자원 제약과 입력·네트워크 조건에서, 제안하는 cross-layer resource manager가 AI 품질을 유지하면서 중요한 AI 이벤트의 deadline-aware delivery를 얼마나 향상시킬 수 있는가?**

이렇게 두면 MQTT/QUIC은 통신 실행 수단이 되고, 연구의 주어는 디바이스의 자원 관리자다. 기존 transport 비교는 유지하되, AI runtime·입력 admission·출력 표현·전송 큐·복구 작업을 연결하는 비교로 확장한다.

## 2. 사용자 문제 정의에서 정교하게 할 부분

- AI 모델 크기와 RAM 점유를 동일시하지 않는다. 가중치는 flash에 있을 수 있고, RAM의 주된 비용은 activation/arena, decode/preprocess buffer, 복사본, task stack 등일 수 있다. 실제 linker 배치와 allocator를 확인해야 한다.
- TFLM은 제공된 tensor arena를 head/temporary/tail 영역으로 관리한다. 추론 횟수를 줄이는 것과 예약된 arena를 줄이는 것은 다르다. 현재 프로젝트도 같은 모델 context를 재사용한다. 따라서 추론 빈도 감소는 CPU·생성량을 줄일 수 있지만 곧바로 arena RAM을 반환하지는 않는다. [TFLM 공식 메모리 설명](https://github.com/tensorflow/tflite-micro/blob/main/tensorflow/lite/micro/docs/memory_management.md)
- 고정 입력 크기의 FOMO가 사람이 나타날 때마다 자동으로 더 많은 RAM을 쓰는 것은 아니다. 이벤트가 inference rate, snapshot 수, feature/raw payload를 바꾸도록 설계돼 있을 때 그 부하 증가를 측정해야 한다. 고정 추론+출력 burst와 추론 빈도 증가+출력 burst를 분리한다.
- 일반 IoT도 알람·오디오·영상·재접속에 따른 burst가 생긴다. 차별성은 burst의 존재 자체보다 **추론의 실행 비용/감지 품질과 통신 부하가 같은 장치의 예산 안에서 결합된다는 점**이다.
- 메모리를 무조건 최대한 채우는 것을 목표로 삼지 않는다. 동일한 제한과 외생 부하에서 정보 가치를 유지하면서 overflow를 막는 것이 목표다.

ESP32-S3에는 capability가 다른 메모리 영역이 있고 총 free bytes만으로 모든 할당 가능성을 판정할 수 없다. internal DRAM과 PSRAM, 필요한 capability, 최대 연속 블록을 따로 측정해야 한다. [프로젝트 버전에 맞춘 ESP-IDF 5.2.2 문서](https://docs.espressif.com/projects/esp-idf/en/v5.2.2/esp32s3/api-reference/system/mem_alloc.html)

## 3. 직접 연결되는 선행연구

아래의 '남는 문제'는 검토한 자료의 범위를 근거로 한 비교이며, 해당 분야 전체에 선행연구가 없다는 주장은 아니다.

| 연구 | 확인한 방법·평가 범위 | 우리 문제에 주는 근거 / 남는 문제 |
|---|---|---|
| MCUNet, 2020; MCUNetV2, 2021 | 모델·추론 엔진 공동 설계, V2의 patch 기반 추론으로 MCU 메모리 제약 대응 | AI 측 메모리 절약 비교군. 통신 큐/이동성의 온라인 예산 중재와는 별도 축 |
| RT-MDM, DAC 2024 | 외부 메모리의 DNN을 분할하고 CPU/DMA 실행 의존성과 segment-group 메모리를 함께 스케줄링. Arduino Nano 33 BLE Sense 사례 및 timing analysis | 가장 가까운 MCU 메모리·실시간 스케줄링 연구. MQTT/QUIC 재전송 버퍼 및 handoff 서비스 공백을 같은 모델에 넣는 확장이 필요 |
| FlexNN, MobiCom 2024 | layer slicing/loading/computing 계획을 메모리 예산에 맞춤. NCNN/ARMv8 중심 공개 구현. §5.6에서 예산 변화에 적응하는 demo | 메모리 예산이 변할 수 있다는 직접 근거. 논문은 예산 변경과 적응 전략의 결정은 개발자에게 맡긴다고 명시. ESP32 즉시 이식 가능한 runtime으로 취급하지 않음 |
| Shisher et al., INFOCOM 2025, Computation and Communication Co-scheduling… | feature 생성과 전송을 함께 선택하는 MGF 정책. AoI에 따른 추론 오차를 목적함수로 사용. 원문은 source 계산 수와 채널 수를 제한하고, 기본 모델에서 다음 슬롯 무오류 수신 가정 | '추론과 통신의 공동 스케줄링' 자체는 선행연구가 존재. 우리 쪽은 실제 byte 단위 메모리, ACK까지의 버퍼 수명, burst와 handoff를 추가로 다뤄야 함. 실제 데이터 기반 수치 평가를 MCU 실기 검증으로 부르지 않음 |
| Sabovic et al., Internet of Things 2023 | 에너지·정확도·시간 제약에 따라 local TinyML/remote inference 선택. battery-less 사람 감지 prototype의 인공광/자연광 실험 | 디바이스가 추론과 전송 중 무엇을 할지 결정한 실기 사례. 주된 제한이 harvesting energy이며 우리 RAM/transport 경쟁과 목적이 다름 |
| Communication-Computation Trade-Off in Resource-Constrained Edge Inference, 2020 preprint | local model 계산 비용과 intermediate feature 통신 비용의 trade-off | metadata/feature/raw 표현 선택의 근거. 완전 로컬 추론 후 결과 전송과 split inference를 같은 방식으로 묶지 않음 |
| Adaptive Device-Edge Collaboration on DNN Inference in AIoT…, 2024 preprint | 추론 도중 local 계산을 어디서 멈추고 offload할지 다단계 결정. digital twin으로 동적 workload 대응, simulation 평가 | 동적 AI/network 선택은 이미 연구됨. MCU 구현 비용과 메모리 안전은 별도 검증이 필요 |
| CoDel, RFC 8289 | 큐 체류시간에 기반한 AQM | FIFO만 상대하면 일반 queue 관리의 효과를 AI 특화 기여로 착각할 수 있음. 네트워크 AQM을 application 객체 큐에 적용할 경우 변경된 의미를 명시 |

각 행의 1차 출처:

1. [MCUNet 저자 원문](https://arxiv.org/abs/2007.10319), [MCUNetV2 저자 원문](https://arxiv.org/abs/2110.15352), [공식 코드](https://github.com/mit-han-lab/mcunet).
2. [RT-MDM 저자 PDF](https://rtclskku.github.io/website/papers/IC202406KLK.pdf), DOI 10.1145/3649329.3655681. 원문 §§2–6을 확인했다.
3. [FlexNN 저자 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2024/07/mobicom24-flexnn.pdf), [공식 코드/평가 안내](https://github.com/xxxxyu/FlexNN). 같은 이름의 hardware accelerator 논문과 구분한다.
4. [MGF 원문](https://arxiv.org/html/2501.04231v1), [INFOCOM DOI](https://doi.org/10.1109/INFOCOM55648.2025.11044625). 특히 §II, §III, §VI를 비교 대상으로 삼았다.
5. [Sabovic et al. 저자 소속기관 기록·accepted manuscript](https://biblio.ugent.be/publication/01HAC4AT1EB85E9T1P0ZHQYZQX), DOI 10.1016/j.iot.2023.100736. 기관 초록과 출판사 section snippets에서 확인한 범위다.
6. [계산–통신 trade-off 원문 초록](https://arxiv.org/abs/2006.02166). 이번 조사에서는 abstract 수준 확인이다.
7. [Digital twin 기반 AIoT 협업 원문 초록](https://arxiv.org/abs/2405.17664). 이번 조사에서는 abstract 수준 확인이며 simulation 주장을 실기로 바꾸지 않았다.
8. [CoDel 공식 RFC](https://www.rfc-editor.org/rfc/rfc8289.html).

추가 검색에서 `Experimental Evaluation of Scheduling Effects on TinyML Inference on ESP32 Using FreeRTOS`라는 2026년 문헌 후보도 나왔지만, 이번 조사에서는 1차 출처 원문/메타데이터 검증을 완료하지 못했다. 관련성은 높아 후속 확인 대상으로만 남기고 결과·수치를 근거로 사용하지 않는다.

## 4. 제안하는 차별화 지점

'AI+통신 joint scheduling 최초'라는 주장은 피해야 한다. 검토한 연구들을 바탕으로 다음 교차점을 실증하는 것이 더 구체적인 기여 후보다.

1. **MCU 내부의 실제 메모리 제약**: 모델 arena와 카메라 버퍼, 애플리케이션 큐, ACK 대기 송신 버퍼, migration 작업 메모리를 함께 계측한다.
2. **전송 이전 단계의 제어**: 큐가 찬 뒤 버리는 방식뿐 아니라, 무거운 decode/inference/snapshot 생성 전에 admission을 결정한다.
3. **AI의 의미를 반영하는 효용**: 전체 FPS 대신 중요한 이벤트 감지와 유효 결과의 deadline 내 도착을 보존한다.
4. **이동성 복구와의 결합**: 기존 프레임 재전송·새 추론·경로 검증이 같은 CPU/RAM을 요구할 때 자원 예약과 우선순위를 조정한다.
5. **실행 가능한 경량 controller**: MCU에서 측정 가능한 상태와 작은 상태 머신/표 기반 정책부터 시작하고, 결정 시간과 자체 RAM 비용도 공개한다.

이는 연구 가설이다. 더 넓은 관련 문헌 검토와 구현·실기 평가 전에는 신규성이나 보장 성능을 확정할 수 없다.

제목 후보: **메모리 제약과 정보 가치를 고려한 이동형 MCU AIoT 디바이스의 추론–통신 공동 스케줄링**.

## 5. 시스템 모델 제안

### 메모리: 영역별 예산과 소유권

각 메모리 영역 p ∈ {internal DRAM, PSRAM}에 대해:

    M_base,p + M_AI_reserved,p + M_preprocess,p(t)
    + M_appqueue,p(t) + M_transport,p(t) + M_recovery,p(t)
    <= B_p - H_p

B_p는 해당 영역에서 연구 대상 작업들이 사용할 수 있는 총 예산이며, 현재 남은 free bytes와 혼용하지 않는다. H_p는 제어/시스템 진행을 위한 reserve다. 각 항은 물리적 allocation의 소유권을 기준으로 중복 없이 집계한다. tensor arena가 이미 input tensor를 포함하면 input을 다시 더하지 않는다. 별도 JPEG/decode 복사본은 별도 allocation이면 포함한다.

'애플리케이션 큐에서 꺼냈다'와 '메모리가 해제됐다'는 다르다. reliable STREAM은 ACK 전까지 데이터를 보관할 수 있다. 따라서 다음 두 상태를 구분한다.

    Q_app(t+Δ) = [Q_app(t) + admitted_bytes - handed_off_bytes - app_drop_bytes]+
    U_tx(t+Δ)  = [U_tx(t) + retained_bytes - ack_released_bytes - abort_released_bytes]+

handed_off_bytes와 retained_bytes는 복사/참조/부분 쓰기 구현에 따라 같지 않을 수 있다. application 큐 상한만으로 디바이스 전체 OOM 방지를 보장했다고 해서는 안 된다.

### CPU와 메모리는 다른 제한

추론 빈도를 낮춰 CPU를 확보해도 고정 arena는 남을 수 있다. 반대로 PSRAM이 넉넉해도 Wi-Fi/TLS의 internal allocation 또는 최대 연속 블록이 부족할 수 있다. controller는 추론 wall time/CPU time, ACK 진행, task deadline과 memory pool을 함께 관측해야 한다.

큐가 일정 속도로 늘어나는 짧은 구간에서만 예측 여유시간을 참고할 수 있다.

    T_fill ≈ remaining_usable_queue_bytes / (admitted_rate - effective_release_rate)

분모가 양수일 때의 근사치다. 핸드오프에서 service rate가 급변하고 내부 transport 메모리가 늘면 정확한 OOM 시각이 아니므로, 이 식 단독을 안전 보장으로 사용하지 않는다.

### 제어 항목

- 빠른 제어: stale/중복 객체 제거, snapshot admission, event metadata 우선, 큐 예약, 복구 probe/제어 트래픽의 예산 확보.
- 중간 속도 제어: sampling/inference 주기, burst 지속시간, JPEG 크기/품질, event별 snapshot 수, metadata/feature/raw 선택.
- 느린 제어: 서로 검증된 모델/입력 profile 전환, arena 재구성. 전환 비용·정확도·중단 시간을 따로 측정한다.

감지해야 이벤트 우선순위를 알 수 있으므로, 과부하 때 inference를 무조건 끄면 중요한 사건을 놓칠 수 있다. 저비용 감지 경로 또는 최소 추론 주기를 먼저 보장하고, 선택적 raw/snapshot 부하를 먼저 줄이는 정책을 후보로 둔다. 이것도 event recall 검증 없이 최적이라고 단정할 수 없다.

MQTT QoS0을 선택해도 reliable QUIC STREAM 자체가 비신뢰 전송으로 바뀌는 것은 아니다. QoS 변경은 전달 계약의 변화이므로 중요한 이벤트의 신뢰성 조건을 고정한 실험과 분리한다.

## 6. 지금 저장소에 대입한 관찰

아래는 소스 관찰이며 이번 턴의 새로운 실기 측정이 아니다.

- `src/components/ai_inference/ai_inference_tflm.cc:132`: 동일 model context가 초기화돼 있으면 arena를 재사용한다.
- 같은 파일 `:171`: PSRAM arena를 우선 할당하며 큰 arena의 internal fallback을 막는 코드가 있다. 이미 AI와 transport 메모리 경쟁을 의식한 수동 규칙이다.
- `src/components/ai_inference/include/ai_inference_model_asset_generated.h:26`: 현재 생성 asset의 arena 설정은 1MiB다. 실제 사용량/최소 필요량과 같다는 뜻은 아니다.
- `src/components/ai_inference/ai_inference_vehicle_model.c`: 모델 바이트 배열은 const이며 길이는 657,088B다. 최종 flash/RAM 배치는 ELF/link map으로 별도 확인해야 한다.
- `src/components/sources/source_async.inc`: AI worker 우선순위를 IDLE과 공유한다. CPU 경합을 처리하는 정적 정책이며 workload/network 기반 온라인 자원 관리자는 아니다.
- 기존 admission C 정책은 영상 큐 중심이다. 모델 선택·추론 주기·tensor 메모리·실제 이벤트 효용을 모두 함께 결정하지 않는다.
- 앞선 host 124회 결과는 transport/큐/복구 분리 검증에 유효하지만 실제 AI 연산과 변하는 모델/전처리 부하를 포함하지 않았다. 이를 이번 연구 질문의 실증 완료로 해석하지 않는다.

## 7. 실험 설계 변경

### 공정한 입력은 '같은 관측 세계'

공동 제어기는 inference 횟수와 생성 payload를 바꿀 수 있다. 따라서 모든 후보에 같은 개수의 AI 출력 메시지를 강제하면 핵심 제어 행동을 금지하게 된다.

**같은 센서 원본 trace, 같은 ground truth 이벤트, 같은 네트워크 trace, 같은 hardware/예산**을 제공한다. 후보가 어떤 inference와 전송을 선택했는지 기록한다. 기존 고정 패킷 재생은 transport-only ablation으로 유지한다. 현장 live 장면은 통제된 replay 평가와 분리한다.

최소 workload 축:

- AI 측: 고정 모델/고정 주기 → 이벤트 시 snapshot burst → 추론 주기+snapshot 동시 burst. 실제 지원 모델/해상도만 사용한다.
- 네트워크 측: 정상 → 대역폭 제한 → RTT/손실 악화 → 단절/주소 변경 → 실제 AP 전환. 각 원인을 독립적으로도 평가한다.
- 시간 관계: AI burst와 네트워크 악화가 겹침 / 앞섬 / 뒤따름 / 독립 발생. 악조건이 항상 상관된다고 가정하지 않는다.
- 메모리 측: 동일한 전체 제한에서 queue cap을 스윕. 내부 DRAM/PSRAM reserve와 fragmentation을 관측한다. 인위적 ballast는 별도 stress test로 표시한다.

권장 비교군:

1. 고정 AI + FIFO, 고정 예산/동일 transport.
2. 고정 AI + deadline/latest/AMR admission: 기존 통신·큐 관리만의 효과.
3. AI-only adaptation: 추론/해상도만 제어하고 통신 정책 고정.
4. network-only adaptation: AI는 고정하고 admission/전송 표현/복구만 제어.
5. 독립 controller 두 개: 서로 상태를 공유하지 않는 AI 및 network 적응.
6. 제안하는 공동 controller: memory pool·중요도·deadline·복구 상태 공유.
7. offline oracle: 미래 trace를 이용한 분석적 상한. 실제 실행 가능한 비교군과 구분.

MGF/RT-MDM/FlexNN은 각각 다른 모델과 플랫폼을 전제로 한다. 직접 원본 실행과 MCU용 adaptation을 구분하고, 모두를 같은 코드로 간단히 치환해 '논문 재현'이라고 부르지 않는다. 필요하면 핵심 결정 규칙의 ablation부터 구현한다.

### KPI

평가의 최상위 분류는 사용자 제안대로 **AI Quality / AI Runtime / Device Resource / Communication**으로 둔다. MLPerf Tiny는 quality·latency·energy를 함께 평가하므로 이 구성의 AI 측 근거가 된다. 다만 아래 네 축 전체가 단일 논문의 표준 지표 세트인 것은 아니다. MCU 메모리 및 이동성 통신 평가를 결합한 본 연구의 구성이다. [MLPerf Tiny 논문](https://arxiv.org/abs/2106.07597), [공식 벤치마크](https://mlcommons.org/benchmarks/inference-tiny/).

| 평가 축 | 채택 지표와 측정 정의 |
|---|---|
| AI Quality | FOMO: centroid matching precision/recall/F1. 클래스 일치·거리 임계값·일대일 매칭·confidence threshold를 고정한다. bounding-box 검출기를 평가할 때는 mAP와 IoU 기준을 명시한다. 동일한 held-out 입력에서 모델 품질을 비교하고, 전체 시간축의 event recall을 별도로 보고한다. |
| AI Runtime | inference latency p50/p95/p99, 실제 inference FPS. 모델 Invoke 구간과 capture→preprocess→inference→postprocess 전체 구간을 분리한다. camera FPS·inference FPS·receiver FPS를 혼용하지 않는다. |
| Device Resource | internal DRAM/PSRAM별 peak allocated RAM·min free heap·최소 largest free block, allocation failure/reboot. 동일 길이 실험의 전체 장치 에너지(J), 필요 시 J/inference 및 J/기한 내 전달된 유효 event. 에너지는 외부 계측하며 주변장치 포함 범위를 고정한다. |
| Communication | enqueue→수신 및 capture/event→수신 p95/p99를 분리. unique application payload goodput, application queue 및 ACK 대기 bytes, queue sojourn time, handover interruption/recovery. network packet loss·정책 drop·실험 종료까지 미수신·deadline miss를 별도 집계한다. |

FOMO는 객체 중심을 학습하는 구조이므로 후처리로 만든 박스를 일반 bounding-box 검출기의 IoU-mAP와 바로 비교하지 않는다. 위 centroid F1 매칭 규칙은 본 실험에서 사전에 고정할 평가 규약이다. [FOMO 공식 설명](https://docs.edgeimpulse.com/studio/projects/learning-blocks/blocks/object-detection/fomo).

네 축을 연결하는 **공동 제어의 주지표**는 `전체 ground-truth event 중 deadline 안에 올바른 결과가 수신된 비율`로 제안한다. 오탐률/precision과 함께 보고해 무차별 검출의 이득을 막는다. 동일 메모리 예산·동일 입력/네트워크 trace에서 이 비율을 비교한다. 이는 본 연구의 목적 지표이며 위 문헌들의 공통 표준이라고 부르지 않는다.

수신 완료 데이터만의 p99는 느린 데이터를 버릴수록 좋아질 수 있다. 따라서 표본 수·반복 횟수·불확실성, 미수신 및 deadline miss를 반드시 함께 제시한다. handover는 링크 단절/경로 변경 기준 시점부터 첫 새 유효 결과 수신까지와, 정상 처리율 회복까지를 구분한다.

다음은 네 축을 실제 실험 질문에 연결한 상세 지표다.

| 목적 | 지표 |
|---|---|
| 중요한 사건을 놓치지 않기 | 전체 ground-truth event 기준 event recall, event→local detection 지연, false alarm rate |
| 유효 정보 전달 | 전체 event 기준 deadline 내 유효 결과 도착률, 생성/선택/폐기/수신을 분리한 utility goodput |
| 메모리 한도 준수 | internal/PSRAM별 peak·min-free·largest-block, queue+transport retained bytes, allocation 실패/OOM/reboot |
| 연산 안정성 | inference p50/p95/p99, deadline miss, CPU idle/runtime, watchdog, controller overhead |
| 정보 최신성 | receiver AoI 또는 task loss vs age. '최신이면 항상 정확하다'는 가정은 실측으로 검증 |
| 이동성 | path validation, 동일 연결 유지, 첫 유효 event/새 입력의 수신, blackout 후 복구 시간 |
| 실제 비용 | 외부 계측 에너지/event, 추론 및 전송당 에너지; RAM이나 CPU 비율을 전력으로 대체하지 않음 |

분모는 scheduler가 버리기 전의 ground-truth event다. 처리한 프레임에서의 accuracy만 보면 inference를 줄여 사건을 놓친 정책이 과대평가된다. 디바이스/서버 시계를 바로 빼지 않고 offset 오차를 처리한다. 디바이스 외부에 기록 가능한 ground truth와 송수신 시각 기준을 먼저 준비해야 한다.

## 8. 현재 결론과 다음 조사·구현 순서

이 방향은 기존 transport 연구를 디바이스 자원 관리 연구 안에 배치할 수 있다. 핵심 기여 후보는 '새 QUIC 자체'가 아니라 **실제 메모리 소유권과 이벤트 효용을 반영해 추론 생성 단계부터 통신 복구까지 중재하는 경량 정책**이다.

우선순위는 (1) 현재 모델/카메라/통신의 메모리 ownership 및 CPU 프로파일, (2) event-labeled 동일 센서 trace, (3) 고정 AI vs 단독 적응 vs 공동 적응, (4) 실제 핸드오프 동시 부하 시험이다. AI 학습 모델 교체·QoS 변화·RTOS 변경·transport 변경을 한꺼번에 넣지 않아야 어느 제어가 효과를 냈는지 판단할 수 있다.
