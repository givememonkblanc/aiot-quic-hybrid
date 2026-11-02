# ESPDet + ROI + QUIC 통합 부하 측정

측정일: 2026-09-21 KST. 단회 pilot이며 반복 실험/transport 비교 결과가 아니다.

## 결론

QUIC에 부하가 없었던 것이 아니다. 기존 `video_admission_allow()`는 전송 큐가 비어 있지 않으면 새 영상을 거절한다. 이 제한으로 송신 부하와 큐 증가가 억제됐다. 제한을 우회하면 AI 지연, 큐 적체, 완성 JPEG 전달 실패가 발생했다. 이는 현재 구현의 결과이며 QUIC 자체가 TCP보다 불리하다는 증거는 아니다.

## 구성과 측정

ESP32-S3, ESPDet cat224, AI core1/priority1, net_tx core1/priority5. 카메라 태스크는 계속 실행하되 AI와 preview 입력은 동일한 64,065-byte 공식 고양이 JPEG를 재생했다. 탐지된 객체 중 최고 점수 ROI 한 장을 MCU에서 JPEG로 인코딩했다. MQTT AI metadata와 QUIC DATAGRAM 영상 조각을 기존 로컬 서버에서 수신했다. 서버 VLM은 실행하지 않았다.

각 단계 45초, 초기 10초 제외. gated 5Hz는 전체 34.62초 시점에 heap guard로 종료되어 분석 구간이 더 짧다. gated와 ungated는 별도 부팅의 순차 실험으로, 순서/연결 상태가 통제된 반복 비교는 아니다. 아래 AI 시간은 전처리·모델·해당 조건의 ROI 처리를 포함하는 작업 완료 시간이다.

| 조건 | AI 평균 / p95 (초) | 영상 제출 / 허용 / 완성 수신 (KiB/s) | 최대 큐 (KiB) | CPU0 / CPU1 (%) |
|---|---:|---:|---:|---:|
| AI + metadata | 0.481 / 0.487 | 0 / 0 / 0 | 0 | 32.6 / 96.1 |
| AI + ROI | 0.799 / 0.821 | 22.4 / 22.4 / 21.9 | 14.2 | 39.9 / 97.7 |
| 제한 유지, preview 1Hz | 1.367 / 1.593 | 75.8 / 64.1 / 64.1 | 10.1 | 55.1 / 99.0 |
| 제한 유지, preview 5Hz (조기 종료) | 2.022 / 2.393 | 319.1 / 80.8 / 80.8 | 56.3 | 62.4 / 99.3 |
| 제한 우회, preview 1Hz | 1.692 / 1.863 | 73.7 / 73.1 / 70.8 | 35.4 | 59.8 / 99.0 |
| 제한 우회, preview 5Hz | 3.095 / 3.164 | 318.6 / 318.6 / 0 | 706.8 | 70.4 / 99.8 |

영상 전송량에는 preview와 ROI가 포함된다. 완성 수신은 연속된 조각으로 재조립되고 JPEG 구조 검증을 통과한 이미지 바이트 기준이다. 제시한 p95는 짧은 관측 구간의 기술통계로, 안정적인 tail 추정으로 해석하지 않는다.

## 왜 완성 JPEG가 0인가

제한 우회 5Hz에서도 영상 조각 payload는 94.1 KiB/s 도착했다. 같은 분석 구간에 송신 측에서 7,150개 조각이 만료됐고 완성 JPEG는 0건이었다. MQTT AI 결과는 11건 수신됐다. 따라서 연결 전체가 죽거나 모든 AI 서비스가 중단된 것은 아니다. 조각 단위 만료가 이미지 완성을 방해하는 현재 전송 경로를 보여준다.

net_tx CPU 점유는 metadata-only 약 0%에서 gated 5Hz 약 43.3%, ungated 5Hz 약 51.5%로 증가했다(한 코어 기준). AI 지연 증가는 실행 경쟁과 일치하지만 scheduling, cache/memory, crypto 각각의 인과 기여를 이 실험만으로 분리하지 않는다.

## 중단 조건과 실패 구간

- gated 5Hz: internal free heap 15,483 B에서 16 KiB 보호 기준에 걸려 중단.
- ungated 10Hz: 시작 6.486초 후 internal free heap 16,119 B에서 중단. 이 짧은 구간의 최대 큐는 1,309,476 B (약 1.249 MiB). 초기 10초 제외 조건을 충족하지 못하므로 정상 구간 표에는 넣지 않았다.
- 10Hz에서 AI 완료는 2회뿐이므로 tail 통계는 보고하지 않는다.
- 실제 OOM이나 watchdog crash를 유발하지 않았다. 내부 SRAM 한계가 먼저 나타났으며 PSRAM에는 여유가 있었다.
- profile8은 admission만 우회한다. 최소 프레임 간격, 큐 용량, 메모리 여유 검사, 2초 조각 만료는 유지했다.
- 마지막 live camera 입력의 10Hz 단계는 보호 중단 때문에 실행되지 않았다.

## 재현 자료와 복원

- [제한 유지 실행](results/integrated-20260920T161314Z/REPORT.md): raw serial, records.jsonl, feed.jsonl, summary.json, outcome.json 및 수신 ROI.
- [제한 우회 실행](results/integrated-20260920T161756Z/REPORT.md): 같은 형식의 원자료. 조기 종료는 outcome.json에 포함.
- 실행: `build_integrated.sh`, `integrated_trial.py`, `analyze_integrated.py`. 빌드 소스/해시: `results/integrated-build-source/`.
- 통합 이미지가 기존 OTA 슬롯에 들어가지 않아 로컬 비공개 전체 플래시 백업 후 임시 8 MiB 배치를 USB로 사용했다. 테스트 뒤 `restore_integrated.sh`로 원래 전체 플래시를 복원했고 esptool hash 검증을 통과했다. [복원 로그](results/integrated-restore-flash.log).
- [복원 부팅 로그](results/integrated-restored-serial.log)에서 원래 4 MB 설정, FOMO AI core0, profile0/probe0/mode2/policy1/video30 및 6144 KiB 명령 ACK를 확인했다. 이후 별도 TinyVLM 작업이 USB 플래시 백업을 시작했으므로 이 문서는 이후 장치 상태를 보장하지 않는다.

## 해석 범위

고양이 고정 입력으로 모델·ROI·통신 통합 비용을 검증한 결과다. 나사 탐지 품질, 실시간 카메라 고부하의 최종 단계, 서버 VLM, GT 기반 deadline 성공률, 에너지, TCP 대비 QUIC 비용은 아직 측정하지 않았다. 이 부하 결과 자체로 새 동적 scheduler의 필요성을 입증하지 않는다.
