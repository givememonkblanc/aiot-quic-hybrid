# ESP32 단일 경로 이식 및 시험

2026-09-20, firmware `mobility-amr-port-v20`.
ELF SHA-256: `680f6c14168c92ef2ced99af1fcf14f5deba9c63e445fcc0c745efcc93b9fac2`.

원본과 별도로 `src/components/dl_qads/video_admission.c`에 고정 크기 상태를 갖는 C admission을 구현했다. 동적 메모리 할당을 추가하지 않았다. 실제 camera JPEG enqueue 직전에 호출하며 콘솔 `policy 0..3`으로 바꾼다. RAM 진단의 `video_policy`로 적용 여부를 확인한다. mode 1은 정책과 무관하게 FIFO 기준선, mode 2는 선택한 admission을 사용한다. 기존 RSSI/RTT 이동성 상태·QUIC migration 경로는 유지한다.

| 번호 | 이름 | 동작 |
|---|---|---|
| 0 | current | 기존 cwnd 잔여량, RTT 기반 예상 전송 시간, 초당 1회 probe |
| 1 | amr_latest | AMR의 media 큐 empty admission + 200ms in-flight budget |
| 2 | amr_rssi | policy 1 + RSSI EWMA/히스테리시스로 열화 시 최대 1프레임/초 admission |
| 3 | amr_quality | policy 2 + RTT EWMA >180ms 열화, ≤150ms 정상 조건 |

모두 같은 payload 예산, PSRAM 여유 128KiB guard, 영상 deadline 2초, 실패 프레임 나머지 조각 폐기, AI 결과 우선 처리를 공유한다. FIFO에는 영상 deadline이 없다. 전체 큐가 비어야 영상 프레임을 넣는 현재 구조는 AMR의 depth/RGB 개별 STREAM 큐와 다르다.

AMR의 rate estimate 대신 현재 ngtcp2에서 얻는 `cwnd/RTT`를 rate proxy로 사용했다. 200ms분으로 환산하며 최소 64KiB, 지표 미확인 시 128KiB다. 이 proxy를 실제 측정 BW라고 부르지 않는다. policy 3은 PQI에서 EWMA와 절대 열화 gate를 가져온 **단일 경로 adaptation**이며 RTT/loss/BW 두 경로 PQI 점수 구현이 아니다.

RSSI는 500ms 간격으로 EWMA(새 표본 45%)를 갱신한다. ≤−75dBm이면 열화, >−67dBm 상태가 연속 1초 유지돼야 복귀한다. 미측정 RSSI는 BAD로 처리하지 않고 복귀 타이머를 중단한다. 전환 상태는 구조체에 저장한다. 두 번째 경로가 없으므로 열화 시 프레임 admission을 줄이되 초당 probe를 유지한다. AP를 자동 변경하거나 요청 FPS를 낮추지는 않는다.

minRTT/RR 두 경로 선택, 미구현 ECF/BLEST/ToF를 구현했다고 주장하지 않는다. BLEST의 STREAM blocking 모델을 영상 DATAGRAM에 그대로 적용하지 않았다.

## 검증 방법

- `test_admission.c`: congestion gate, 큐 적체, in-flight 상한, unknown RSSI, 열화 probe, 연속 GOOD 복귀, RTT 열화, 정책 변경 상태 초기화를 검사. ASan/UBSan 및 `-Wall -Wextra -Werror` 통과.
- 카메라/AI OTA 빌드와 non-camera 연구 빌드 확인. 배포 이미지/ELF는 `../ota/results/amr-port-v20/`.
- `live_trial.py`: 실제 보드에서 FIFO와 policy 0..3을 순서/역순으로 각 30초씩 실행. 30FPS 요청, 1024×1024/q10, payload 6MiB 유지. 실제 캡처 FPS는 별도 집계한다.
- 정책값·mode·FPS·예산이 RAM telemetry에서 일치한 표본만 집계. 각 구간의 처음 10초는 수신률 평균에서 제외한다. RAM은 5초 주기, 영상 수신률은 10초 rolling window다.
- 재부팅, RAM telemetry 25초 중단, 내부 free heap 16KiB 미만이면 중단. 종료 시 mode 2 / policy 0 / 30FPS 요청 / 6MiB 예산 복원.
- 두 번째 반복은 역순이지만 같은 live 장면이나 연결 상태를 재현한 것은 아니다. 작은 FPS 차이에 통계적 우열을 부여하지 않는다.

재실행:

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Isrc/components/dl_qads/include experiments/amr_comparison/test_admission.c \
  src/components/dl_qads/video_admission.c -o /tmp/test_amr_admission
/tmp/test_amr_admission
python3 experiments/amr_comparison/live_trial.py --seconds 30 \
  --output experiments/amr_comparison/results/UNIQUE_RUN
```

## v20 결과와 안정성 수정

`results/live-v20-run01`의 10구간(5조건 × 순서/역순), 총 300초 시험은 재부팅 없이 완료됐다.

| 조건 | PSRAM 최고점 | 완성 영상 FPS 평균 | AI 결과 FPS 평균 |
|---|---:|---:|---:|
| FIFO | 98.08% | 0.710 | 0.003 |
| current | 30.42% | 0.715 | 0.660 |
| amr_latest | 30.57% | 0.748 | 0.668 |
| amr_rssi | 30.67% | 0.718 | 0.643 |
| amr_quality | 29.81% | 0.663 | 0.630 |

메모리 적체와 AI 결과 전달 차이는 컸지만 영상 FPS의 작은 차이로 승자를 선언하지 않는다. 실제 캡처는 FIFO 평균 4.69FPS, adaptive 정책 약 6.81~7.47FPS였다. 요청 30FPS 달성 결과가 아니다.

v20 시리얼 기록에는 IDLE0 watchdog 경고 6회와 MQTT 오류 9회가 있다(기록 전체 구간 기준). 경고 backtrace는 camera JPEG memcpy 및 TFLM convolution 실행 위치를 포함한다. 메인과 AI가 모두 priority 1로 CPU 0을 사용해 idle 실행을 밀어낼 수 있다. v21은 AI worker만 `tskIDLE_PRIORITY`로 변경하여 기존 time slicing에서 idle이 실행될 기회를 준다. watchdog을 끄거나 timeout을 늘리지 않았다. 모든 정책에 동일하게 적용하고 다시 측정하며 v20/v21 수치를 합치지 않는다. AI 처리량에 미치는 영향도 기록한다.


## v21 최종 비교 결과

배포 ELF SHA-256: `321c7a5e4e2abca7f05cfe87565ddc6a4ce9afd7e34db7c2265fe7fb46abc0dc`.
`results/live-v21-run01`: 5조건 × 순서/역순, 각 20초, 총 200초 완료. v20의 각 30초 결과와 합치지 않았다. 처음 10초를 제외한 수신률 평균이다. 구간이 짧아 정책별 성능 순위의 근거로 사용하지 않는다.

| 조건 | PSRAM 최고점 | 실제 캡처 FPS 평균 | 완성 영상 FPS 평균 | AI 결과 FPS 평균 |
|---|---:|---:|---:|---:|
| FIFO | 97.28% | 8.18 | 0.64 | 0.00 |
| current | 31.02% | 9.37 | 0.58 | 0.15 |
| amr_latest | 30.69% | 9.89 | 0.65 | 0.18 |
| amr_rssi | 30.44% | 9.64 | 0.62 | 0.17 |
| amr_quality | 30.30% | 10.17 | 0.66 | 0.18 |

비교 중 재부팅·heap corruption·IDLE watchdog 경고는 관찰되지 않았다. MQTT timeout/reconnection은 남아 있다. AI background 우선순위 변경은 캡처에 CPU 여유를 주지만 AI 결과 빈도를 약 0.15~0.18Hz로 낮췄다. 안정성 개선의 대가이며 전체 처리량이 개선됐다고 주장하지 않는다. FIFO의 AI 0은 짧은 관측 구간에서 결과가 도달하지 않은 것으로, AI가 실행되지 않았다는 뜻은 아니다.

30FPS 요청은 유지했지만 실제 캡처/완성 영상 30FPS를 달성하지 못했다. 실제 AP 간 이동 또는 RSSI 경계 주행을 실시하지 않았으므로 RSSI 정책의 이동성 이점도 아직 실증하지 않았다. 히스테리시스 분기 자체는 호스트 C 테스트로 검증했다.


같은 AP 재연결 smoke test(`results/handoff-v21.json`)에서도 재부팅 없이 RAM telemetry가 복구됐고 receiver connection ID가 유지됐다. 시험 후 telemetry 지연은 약 5.1초였다. AP 간 handoff 지연이나 무중단 영상 성능을 입증하는 시험은 아니다.

최종 실행 설정은 **mode 2 / policy 1(amr_latest) / 목표 30FPS / 큐 예산 6MiB**다. NVS에는 저장하지 않았으며 재부팅하면 기본값으로 돌아간다. RSSI 경계가 없는 현재 실내 연결에서 RSSI 우위를 주장하지 않고, AMR admission 이식본으로 운용한다.
