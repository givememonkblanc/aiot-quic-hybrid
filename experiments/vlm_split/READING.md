# VLM/split 초기 문헌 지도

확인일: 2026-09-21. 초기 선정 목록이며 체계적 전수조사 또는 재현 완료 목록이 아니다. 논문 보고값은 우리 보드의 측정값과 분리한다.

| 자료 | 확인한 내용 | 우리에게 필요한 검증 |
|---|---|---|
| [ESP-Detection, 공식 구현](https://github.com/espressif/esp-detection) | ESPDet-Pico 학습·양자화·ESP-DL 배포 경로, S3 지원. 공식 224 입력 고양이 예제 S3 latency 126.2ms. IDF 5.3 이상 요구 | 동시 Wi-Fi/QUIC 비용, 나사 데이터 학습, 실제 RAM/flash, ROI 생성 비용. 공식 예제 성능을 현재 시스템 성능으로 대입하지 않음 |
| [TinyVLM, 2603.00136](https://arxiv.org/html/2603.00136v1) | 작은 인코더와 사전 계산 텍스트 임베딩, Matryoshka 차원 선택. S3 수치는 예상치, 코드/가중치 공개 예정 문구 | 공개 artifact 부재로 직접 교체 보류. [기존 검토](../tinyvlm/PORT_STATUS.md). 일반 VLM 토큰과의 호환성은 별도 문제 |
| [CoVSpec, 2605.02218](https://arxiv.org/abs/2605.02218) | device-edge VLM 협력에서 visual token reduction과 speculative decoding 결합. 페이지상 Globecom 2026 제출 프리프린트 | 'device'의 하드웨어와 draft model 요구량을 원문에서 추출해야 함. MCU 적용 가능하다고 간주하지 않음 |
| [Compressing AI Traffic, 2609.01200](https://arxiv.org/abs/2609.01200) | Qwen3-VL-8B의 visual tokens와 DeepStack 특징을 압축해 downstream QA를 평가 | tensor 복원 오차만이 아닌 task quality–bytes 평가를 참고. 8B VLM visual encoder의 MCU 실행 증거는 아님 |
| [Progressive Feature Transmission, 2112.07244](https://arxiv.org/abs/2112.07244) | 서버가 정보의 유용성과 통신 비용에 따라 특징 선택·전송 종료를 결정하는 split inference 연구 | feature selection/중단 결정은 기존 연구 축임. VLM 및 MCU 제약에서 무엇이 다른지 확인 필요 |

따라서 split inference 전체가 2025–26년에 시작됐다고 주장하지 않는다. 최근 VLM 협력 추론과 오래된 split/feature transmission 연구를 연결해서 공부한다.

각 논문 다음 읽기 항목: 과제, 실제 endpoint 하드웨어, 공개 코드/가중치, encoder/decoder 경계, tensor shape/precision/bytes, adapter 학습, 네트워크 조건, 지연 측정 경계, 정확도 GT, baseline, failure mode, 재현 가능 여부. 추정값·시뮬레이션·실측을 구분한다.
