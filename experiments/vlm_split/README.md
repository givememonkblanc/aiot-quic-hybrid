# MCU–Edge/Cloud VLM 협력 추론

2026-09-21 방향 전환. 이 문서가 현재 연구 우선순위의 기준이다.

진행 상황: [ESPDet 첫 보드 검증](PROBE_RESULTS.md)에 이어 [ESPDet + ROI + QUIC 통합 부하 측정](INTEGRATED_RESULTS.md) 완료. admission 우회 시 큐 적체·JPEG 완성 실패와 내부 RAM 보호 중단을 확인했다. 원래 전체 플래시 복원과 부팅 ACK를 확인했으며, 마지막 live-source 고부하 단계는 실행하지 못했다. 서버 VLM은 현재 ROCm 커널 호환 문제를 확인했으며 아직 실행 검증 전이다.

추가 확인: [30 FPS 개선 시험](../video30/RESULTS.md). 메모리 할당 경로와 전송 대기를 수정했지만, 현재 1024×1024 실영상의 완성 수신은 약1.11 FPS로 목표 미달이며 디코딩 버퍼 실패도 남아 있다. 시험 빌드는 상시 적용하지 않았다.

연구 질문: **동일한 시각 과제와 자원·네트워크 조건에서 MCU가 어디까지 처리하고 어떤 표현을 서버에 전달해야, 요구 정확도와 응답시간을 가장 적은 통신·에너지 비용으로 만족하는가?**

기존 FOMO/QUIC/core-priority 연구는 기준 실험으로 보존한다. 한 고정 부하에서 정적 설정이 효과적이었다는 결과이지 모든 스케줄링 문제가 해결됐다는 결과는 아니다. 동적 스케줄러의 필요성을 만들기 위한 추가 부하 실험은 현재 우선순위에서 내린다. [측정 결과](../aiot_resource_management/SWEEP_RESULTS.md).

## 구분할 실행 방식

| 비교군 | MCU 역할 | 서버 입력/역할 | 선행 조건 |
|---|---|---|---|
| Local-only | 탐지/분류와 결정 | 결과 기록 | 해당 과제를 로컬 모델로 평가 가능 |
| Full-image | 캡처/JPEG | 전체 이미지로 VLM 판단 | 기준 VLM·프롬프트 고정 |
| ROI-image | 탐지 + crop/JPEG | ROI 이미지로 VLM 판단 | 실제 MCU crop/인코딩 비용 포함 |
| Metadata | 클래스/좌표/신뢰도 생성 | 구조화된 정보로 판단 | 원본 시각 정보가 없는 한계 평가 |
| Feature split | 인코더 일부 또는 전체 | 호환 특징을 받아 후속 연산 | 공개 가중치, 분할 인터페이스, 필요한 어댑터 학습 |
| Escalation | 로컬 결정 또는 서버 질의 | 어려운 입력에 응답 | 로컬 신뢰도 보정과 고정 임계값 기준선 |

ROI/metadata 전송은 협력 파이프라인이며 같은 모델 내부를 분할하는 feature split과 다르다. 임의 ESPDet feature나 작은 이미지 임베딩을 VLM visual token으로 그대로 넣을 수 있다고 가정하지 않는다. 작은 embedding이 공간 정보나 질문에 필요한 세부 정보를 보존하는지도 검증 대상이다. 특징의 바이트 수가 JPEG보다 작다는 보장도 없다.

## 첫 실험 과제

턴테이블 위 컴퓨터용 나사의 존재·개수·큰 유형 구분 중 실제 촬영 해상도로 정답을 부여할 수 있는 과제를 먼저 고정한다. 미세 나사산/정확한 규격 추정은 근거 없이 목표에 넣지 않는다. VLM이 필요한지는 측정으로 판단한다. 단순 과제에서 로컬 detector로 충분하면 그 결과도 채택한다.

동일 녹화 입력과 event_id/GT를 사용해 Full-image → ROI-image → Metadata를 먼저 비교한다. 탐지기가 놓친 객체는 최종 실패로 포함한다. ROI 성공 사례만 골라 평가하지 않는다. Oracle ROI는 위치 GT를 사용한 상한 비교로만 분리한다. 학습/검증/테스트는 프레임 무작위 분할 대신 물리적 나사와 촬영 세션 단위로 나눠 누수를 막는다. 이후 실제 턴테이블 재촬영으로 검증한다.

1. ESPDet 독립 실행: 공식 모델로 실행/메모리/출력 검증. 공식 고양이 모델은 나사 탐지 모델로 간주하지 않는다. 나사 모델은 라벨링·학습·독립 평가 필요.
2. 서버 VLM: 실행 가능한 호스트·가속기·모델·라이선스를 확인하고 한 모델/프롬프트/출력 JSON 스키마를 고정. 현재 AMD GPU의 ROCm 실제 커널 실행에서 HIP invalid device function 오류를 확인했으므로 가속 실행 환경 검증이 남아 있다.
3. 같은 입력에서 전체 JPEG와 detector ROI를 서버에 입력해 품질–전송량 비교. 호스트에서 crop한 예비 실험은 MCU 비용 측정으로 표시하지 않는다.
4. ROI/crop/재인코딩까지 MCU에서 수행한 뒤 capture→decision 전체 지연과 자원 측정. 기존 QUIC 경로는 우선 고정해 표현 선택 효과를 분리.
5. 공개·호환 encoder/decoder가 확보될 때만 feature split 추가. TinyVLM은 현재 배포 가중치 미확보로 의존성에서 제외하고 개념 연구 대상으로 유지.
6. Local-only, Always-offload, 고정 ROI, 고정 신뢰도 임계값을 먼저 비교. 동적 선택은 이 기준선으로 해결되지 않는 영역이 확인된 뒤 검토.

## 측정 기준

- Task quality: GT 기준 precision/recall/F1, counting error 또는 정답률. 탐지 AP와 VLM 최종 정답률은 별도. 거절/모름 응답과 coverage도 기록.
- Latency: capture→final decision p50/p95/p99, completion/timeout 수, 전체 GT event를 분모로 한 deadline 내 올바른 결정 비율.
- Communication: 송신 payload 및 실제 wire bytes/event, 완성 요청/응답 goodput, offload 비율, 재전송·fallback 비용.
- Device: internal RAM/PSRAM peak, min free heap, CPU/core, 전처리/추론/인코딩 단계 지연. 에너지는 계측기 확보 전 미측정 처리.
- Server: 모델/정밀도/프롬프트/디코딩 설정, 요청 큐 지연, inference latency, accelerator memory, 가능할 때 비용/에너지.
- Capture/송신/응답은 MCU 동일 시계로 E2E 측정 가능. 서버 처리 시간은 서버 내부 시계로 측정하고, 시계 동기화 없는 MCU↔서버 timestamp 차이를 one-way latency로 쓰지 않는다.
- 모델/입력/표현/transport/펌웨어 해시를 기록. 누락·시간초과·잘못된 JSON도 결과에서 제외하지 않는다.

## 현재 기술 제약

ESPDet 공식 문서는 ESP-IDF 5.3 이상을 요구한다. 현재 /home/ryzen395/esp-idf는 5.2이므로 별도 SDK/빌드 디렉터리에서 검증하며 기존 QUIC 환경을 일괄 업그레이드하지 않는다. ESP-DL 모델과 현재 TFLM FOMO는 런타임/후처리 교체가 필요하다. [공식 배포 안내](https://github.com/espressif/esp-detection).

현재 flash 물리 크기는 부팅 로그상 8 MiB이지만 앱은 4 MiB 파티션 배치를 사용한다. 큰 모델 통합 전에 실제 linked size와 OTA 슬롯을 점검한다. 통합 시험에서는 전체 플래시를 비공개 백업한 뒤 임시 8 MiB 파티션 배치를 사용하고 원래 상태로 복원했다. 외부 클라우드 이미지 업로드는 수행하지 않았다.

## 다음 연구 판단

ROI가 full-image보다 작아도 문맥 손실 때문에 정확도가 낮아질 수 있다. feature encoding 비용이 통신 절감보다 클 수도 있다. VLM이 local-only보다 유용한 추가 판단을 하지 못할 수도 있다. 어느 경우든 관측 결과를 채택한다. 연구 기여는 MCU 플랫폼에서 재현되는 품질–지연–자원 상충과 이를 개선하는 방법에서 정하며, 새 scheduler나 split point 알고리즘을 미리 정답으로 두지 않는다.

문헌 공부와 플랫폼 개발은 두 작업 흐름으로 진행한다. 초기 문헌 목록은 [READING.md](READING.md).
