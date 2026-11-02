# ESPDet 첫 보드 검증 — 2026-09-21

ESP32-S3에서 공식 ESPDet-Pico 224 INT8 모델과 실제 ROI crop/JPEG 인코딩 실행에 성공했다. 시험 후 기존 FOMO/QUIC 펌웨어로 자동 복귀했고, 실시간 수신에서 mode2/policy1/target30/budget6144 KiB 설정을 확인했다. **나사 모델, 실시간 카메라 ESPDet, 서버 VLM까지 통합 완료한 결과는 아니다.**

## 조건과 결과

공식 고양이 두 마리 이미지(640×480 JPEG 64,065B), detector 입력224×224, ESP-DL3.3.11, ESP-IDF5.3.6, CPU240MHz/PSRAM40MHz. 카메라·Wi-Fi·QUIC 비활성의 독립 시험. 모델 가중치는 수정하지 않았다. 워밍업3회 후20회, 동일 이미지 반복. 모델 초기 로드179.0ms는 반복 추론에서 제외.

| 단계 | 평균 | p95 |
|---|---:|---:|
| JPEG→RGB 디코딩 | 101.02ms | 101.03ms |
| 탐지(전처리+모델+후처리) | 191.48ms | 191.49ms |
| JPEG→탐지 완료 | 292.50ms | 292.52ms |
| 두 ROI crop+JPEG 인코딩 | 163.98ms | 163.99ms |
| 위 처리 단계 시간 합계 | 456.48ms | 456.49ms |

20회 모두 탐지2건, 출력 score 약0.893/0.867. 이는 공개 예제의 smoke test이며 F1/mAP 검증이 아니다. 추론191ms의 역수 약5.22Hz, 디코딩 포함 약3.42Hz, ROI 포함 약2.19Hz는 처리시간 역수일 뿐 실제 live capture/전송 FPS가 아니다. SDK/PSRAM/입력 경로가 다른 기존 FOMO와 모델만의 공정한 속도 비교로 사용하지 않는다.

ROI는451×302/359×394, quality80 JPEG19,123B/16,951B. 합36,074B로 원본보다43.7% 작다. 첫 회에 두 ROI 모두 다시 디코딩되고 크기가 일치함을 확인했다. 좌표는 clamped exclusive upper bound이며 원본 JPEG의 품질/서브샘플링과 ROI 재인코딩 조건이 동일하다고 보장하지 않는다. VLM 판단 품질 및 동일 화질에서의 압축 효과는 미측정이다.

## 자원

실행 전 free: internal305,751B / PSRAM8,385,916B. allocator low-water: internal194,247B / PSRAM5,640,112B. 모델 제거 후 free: internal305,359B / PSRAM8,385,916B. 짧은 반복에서 PSRAM의 누적 잔류는 관찰하지 않았지만 장기 누수 검증은 아니다. Low-water에는 워밍업 ROI 재디코딩 검증이 포함되며, 여러 heap 영역 최소값의 합이므로 정확한 동시 peak 또는 모델 tensor arena로 해석하지 않는다.

## 빌드·배포 검증

전체 ESP-DL 등록/색상 변환 포함 시 OTA 슬롯 초과. 실제 모델 FlatBuffer에서7종 연산자를 추출해 등록하고 RGB888 경로만 남겼다. 최종 binary1,504,496B, 기존 OTA 슬롯2,031,616B 이내. 가중치/연산 정의 변경 없이 사용하지 않는 기능을 제외했다. [재현 코드](espdet_probe/README.md), [해시·버전](PROBE_MANIFEST.json).

첫 OTA는 프로젝트명 검증에 거절되어 장치는 기존 앱에 남았다. 시험 앱의 프로젝트명을 기존 aiot_quic으로 맞춘 두 번째 배포가 통과했다. 검증 로직·부트로더·파티션·NVS는 변경하지 않았다. 임시 앱은 완료 후 OTA rollback으로 이전 정상 앱으로 복귀한다.

## 다음 비교에서 주의할 점

ROI 절감27,991B에는 추가 계산비용이 있다. 로컬 탐지가 원래 필요하면 추가 crop/encode 약164ms와 절감량을 비교한다. Full-image offload가 로컬 탐지를 전혀 하지 않는 기준선이면 **디코딩+탐지+ROI 약456ms 전체**가 추가비용이다. 어느 쪽 기준선인지 먼저 정해야 한다. 전송과 계산 overlap, 서버 전처리 비용, ROI 개수/중복, 문맥 손실도 별도로 측정한다. 이 예제의43.7%를 나사 장면의 절감률로 일반화하지 않는다.

서버에는 AMD Strix Halo GPU와 Qwen2.5-VL-7B 캐시(5 weight shards)가 있다. 현재 기본 PyTorch2.5.1/ROCm6.2는 GPU를 열거하지만 실제 작은 행렬 연산에서 `HIP error: invalid device function`으로 실패했다. 따라서 서버 VLM 실행·품질·지연은 아직 검증하지 못했다. 기존 환경을 전역 변경하지 않고 호환 런타임을 별도로 검증해야 한다.

다음 순서: 서버 VLM 실행 환경 해결 → 동일 입력 Full-image/ROI 품질 비교 → 나사 데이터/라벨 준비와 ESPDet 학습 → 카메라·통신 동시 통합. 현재 보드의 정상 서비스는 기존 FOMO다.

원시 기록: [실행 결과](results/probe-20260920T155601Z/outcome.json), [측정 JSON](results/probe-20260920T155601Z/summary.json), [serial](results/probe-20260920T155601Z/serial.log). 과거 실패는별도 results/probe-20260920T155410Z에 보존.
