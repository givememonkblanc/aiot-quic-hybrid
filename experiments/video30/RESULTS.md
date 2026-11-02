# 30 FPS 개선 시험 결과 — 2026-09-21 KST

**30 FPS 목표 미달.** 코드의 프레임 간격·태스크 대기와 메모리 할당 경로를 수정해 시험했지만, 현재 1024×1024 JPEG + MCU AI + QUIC 구성에서 완성 영상 수신은 약 1.11 FPS였다. 시험 빌드에는 실제 카메라 AI 디코딩 버퍼 할당 실패가 남아 있으므로 상시 적용 대상으로 판단하지 않았다.

## 유효한 최종 측정

[원자료](results/optimized-20260920T165833Z/outcome.json), [추가 지표](results/optimized-20260920T165833Z/video30-summary.json), [재조립 이미지 목록](results/optimized-20260920T165833Z/complete_images.json).

각 조건 45초, 초기 10초 제외. AI core1/priority1, TX core1/priority5. 내부 RAM 16 KiB / PSRAM 128 KiB 보호 기준 유지. 최종 두 구간은 이 기준으로 완료했으며 panic/watchdog reset은 관측되지 않았다.

| 조건 | 입력 FPS | 완성 preview FPS | 완성 영상 goodput | AI 성공 작업 평균 | 최소 internal free | 최대 큐 |
|---|---:|---:|---:|---:|---:|---:|
| 고정 640×480 JPEG, preview 5Hz + ROI | 9.97 | 1.57 | 100.83 KiB/s | 1.770초 | 32,903 B | 42,241 B |
| 실제 1024×1024, 목표 capture/preview 30Hz + AI | 9.18 | **1.11** | 135.69 KiB/s | 2.605초 | 60,747 B | 120,028 B |

- 고정 구간: 분석 창에서 preview 55장, ROI 5장, MQTT AI 결과 19건. 영상 송신 실패/만료 counter 증가는 0. admission 거절 132회, 간격 거절 1회.
- 실제 카메라 구간: preview 39장, MQTT AI 결과 6건. 송신 실패 counter +1, 만료 조각 +61. preview-only profile5라 ROI 전송은 이 구간에 포함되지 않는다.
- 최종 실행 로그 전체에서 JPEG decoder 출력 버퍼 할당 실패 메시지 **38회**. 실패는 AI 성공 작업 평균에서 제외되므로, 그 평균만으로 서비스 품질 개선을 주장하지 않는다. 정확한 구간별 실패 수는 serial 타임스탬프로 확인 가능하다.
- 고정 JPEG 및 실제 카메라는 입력 크기와 내용이 다르므로 goodput과 FPS를 서로 직접 비교해 효율 우열로 해석하지 않는다.
- 별도 성공 연결의 camera-only warmup(15–30초, profile1)은 약 11.36 FPS. 이는 현재 센서 설정/노출/소프트웨어 경로에서의 측정이며, 센서의 모든 설정에 대한 절대 상한은 아니다. 초기 TLS 재연결 실패 중 관측했던 약11.5 FPS와 구분한다.

## 이전 pilot과의 제한적 비교

이전 gated 5Hz pilot의 영상 goodput 80.8 KiB/s → 이번 고정 JPEG 조건 100.83 KiB/s, AI 평균 2.022초 → 1.770초. 여러 수정이 함께 들어간 별도 부팅 단회 실험이며, 네트워크 조건도 동일하게 강제하지 않았다. 약25% goodput 차이를 특정 수정 하나의 인과 효과로 단정하지 않는다. 30 FPS 달성 증거가 아니다.

## 패킷 경로 계측

최종 고정 JPEG 구간의 datagram API 호출당 평균 wall time:

| 항목 | 평균 |
|---|---:|
| 연결 mutex 대기 | 3.542 ms |
| ngtcp2 패킷 생성·암호화 | 2.277 ms |
| UDP send | 0.784 ms |
| 전체 호출 | 6.730 ms |

실제 카메라 구간은 각각 1.472 / 2.590 / 0.823 / 4.996 ms였다. 이 값들은 CPU 실행시간과 같지 않으며 내부 스케줄링/메모리 stall을 분리하지 않는다. 이전 AES 경로의 5.909 ms packet-generation 표본은 단일 관측이라 최종 구간 평균과의 정량적인 암호화 속도 비교로 쓰지 않는다.

## 수정 및 검증

1. 41 ms admission 간격을 33 ms로 수정. 메인 루프 20→1 ms, QUIC process 5→1 tick. TX는 매 조각마다 1 tick을 쉬던 대신 최대 3 ms 작업 후 yield한다. watchdog/혼잡 제어/만료 정책은 유지했다.
2. FreeRTOS의 wolfSSL `XMALLOC` 매핑이 `wolfSSL_SetAllocators`를 우회함을 코드에서 확인. 선택적 `XMALLOC_USER`의 malloc/free/realloc을 같은 PSRAM 우선 allocator에 연결했다. ngtcp2 pool도 PSRAM 우선으로 구성했다.
3. TLS 1.3 ChaCha20-Poly1305 협상과 실제 수신을 확인했다. 암호화나 기존 인증 설정을 끄지 않았다.
4. AES-GCM 정답 벡터, 복호화, 변조 tag 거절 검증에서 software 경로 PASS. hardware AES + GCM_WORD32 조합은 FAIL이라 사용하지 않았다. 이 조합 시험만으로 실패 원인을 hardware AES 하나에 귀속하지 않는다.
5. Main stack 20→12 KiB: 최종 실측 최소 여유 3,624 B. net_tx 7,452 B, camera_ai 9,720 B 여유를 관측했다. 모든 입력/재연결 조건의 WCET/최악 스택 증명은 아니다.
6. ROI encoder 입력을 16-byte aligned PSRAM으로 할당한다.
7. [최종 소스·펌웨어·SDK 해시](results/final-source/hashes.json)를 보존했다. `build.sh`, `trial.py --tx-only`, `summarize.py RUN_DIR`로 재현 경로를 기록했다.

## 실패한 예비 실행도 보존

- 16:36/16:39 UTC: TLS 할당/연결 실패. receiver feed가 없으므로 throughput 결과로 무효 처리.
- 16:42 UTC: hardware AES + 32-bit GHASH known-answer FAIL. 연결 시작 전 거절.
- 16:45 UTC: software self-test PASS, TLS 할당 실패가 남아 무효.
- 16:48 UTC: allocator 수정으로 실제 송신 확인, internal free 16,199 B에서 보호 중단. settling 미충족.
- 16:51 UTC: ChaCha20 연결/송신 확인, ROI 처리 도중 10,099 B에서 보호 중단. settling 미충족.
- 16:58 UTC: stack 조정 후 위 두 45초 구간 완료. 실제 카메라의 decoder allocation 실패는 미해결로 기록.

## 다음 결정에 필요한 조건

현재 1024×1024 해상도를 유지했다. 64,065-byte 시험 JPEG조차 30 FPS면 payload 15.38 Mbit/s가 필요하다. 큐 크기를 늘리거나 만료를 없애는 것으로 이 처리율 차이를 해결할 수 없다.

30 FPS 우선이라면 미리보기의 해상도/압축률/표현을 바꾸는 실험이 필요하다. 원본 증거사진을 별도 보존하는 구성도 비교 가능하지만, 640급으로 바꾸기만 하면 30 FPS를 달성한다고 아직 보장할 수 없다. 실카메라 RGB 디코딩 버퍼의 연속 공간/재사용도 후속 검증 대상이다. 이번 결과로 동적 scheduler의 필요성이나 QUIC 대비 다른 transport의 우열을 주장하지 않는다.

## 장치 복원

원래 전체 8 MiB 플래시 복원을 완료했고 esptool hash 검증을 통과했다. [복원 로그](results/restore-flash.log). [복원 후 콘솔](results/restored-serial.log)에서 profile0/probe0/mode2/policy1/video30·6144 KiB 설정 ACK와 FOMO AI core0 실행을 확인했다. NVS에 새 설정을 저장하지 않았다. 임시 8 MiB ESPDet/ChaCha20 이미지는 상시 적용하지 않았으며, 원래 파티션·펌웨어를 유지한다.
