# 영상 전송 간섭 분해: 측정 결론

측정: 2026-09-20 UTC / 2026-09-20~21 KST. Core0 8구간 + core1 4구간, 각45초, 처음15초 제외. 새 resource manager를 만들지 않고 기존 경로를 분해했다.

## 결과

| 조건 | AI 완료 시간(구간별 평균) | AI 태스크 CPU 실행시간 | CPU0 / CPU1 사용률 |
|---|---:|---:|---:|
| E1: JPEG 캡처+AI+메타데이터, 영상 폐기 | 1.60–1.73초 | 0.524–0.546초 | 67–68% / 0.2% |
| E2: 영상 chunk 복사·큐 삽입·즉시 폐기 | 1.60–1.78초 | 0.522–0.550초 | 67–69% / 0.4% |
| E3: 실제 QUIC 영상 전송, 추가 제한 없음 | 2.75–4.42초 | 0.567–0.659초 | 79–85% / 43–55% |
| E4: 선택한 흐름만 512kbit/s +50ms 제한 | 2.97–2.99초 | 0.573–0.581초 | 약80% / 약40% |
| E5: AI core1, 영상 폐기 | 1.12초 | 약0.56초 | 약39% / 약49% |
| E5: AI core1, 실제 영상 전송 | 2.05–2.06초 | 약0.58초 | 61–62% / 약71% |

모든 영상 비교는 초당1회 제출 시도 상한을 사용했다. 이전 unrestricted-preview 4.8초 결과와 동일한 부하가 아니다. E1에서도 AI는 동작한다. OV2640이 JPEG를 출력하므로 MCU의 별도 JPEG 인코더 태스크는 없다.

## 원인에 대한 근거

- E2는 E3보다 많은 영상(분석 창당28–29장 vs14–15장)을 큐에서 처리하고도 E1 수준이었다. 단순 chunk allocation/copy/enqueue/dequeue만으로 큰 AI 지연을 설명하기 어렵다.
- 실제 전송에서 net_tx는 한 코어 기준 약38–49%를 점유했다. main(카메라 획득 + MQTT + QUIC 수신/ACK/타이머 처리)은 약23–26%에서38–45%로 증가했다. AI가 받는 CPU 시간 비중은 약31–32%에서14–21%로 감소했다.
- AI 자체 scheduled CPU time도 약8–21% 증가하므로 모든 차이를 순수 대기만으로 환원하지 않는다. CPU 시간에는 메모리 stall 및 interrupt attribution 등도 섞일 수 있다. 그러나 완료 시간 증가가 훨씬 크고, core 이동으로 줄어들어 실행 기회/배치의 영향이 크다는 근거가 있다.
- AI 우선순위0, net_tx 우선순위5. main 기본 우선순위1이며 전송 구간에서5인 표본도 있다. 코드상 QUIC mutex를 main/net_tx가 공유한다. 우선순위 상속의 영향은 후보 메커니즘이며 mutex별 대기시간을 직접 계측한 결과는 아니다.
- 현재 QUIC 영상 경로와 태스크 구성의 비용을 좁힌 실험이다. QUIC 프로토콜 전체가 TCP보다 느리다거나 암호화만이 원인이라는 주장은 할 수 없다.

## 비교의 한계

- 실시간 장면이므로 입력 바이트가 동일하지 않다. E3 첫/두 번째 구간의 수락 JPEG 평균은 약135.6/166.4KiB로 달랐고 두 번째는 제한 구간 이후다. E4가 E3보다 언제나 나쁘다고 주장하지 않는다.
- E2는 네트워크 상태 admission gate를 우회한다. E3/E4는 원래 admission을 유지한다. 동일 제출 빈도와 실제 수락량을 구분한다.
- E4는 서버 ingress에서 해당 ESP32→UDP14567 흐름만 제한했다. netem queue에서75/105개 패킷이 폐기됐고, 이는 인위적인 random loss 설정이 아니라 제한 큐의 overflow다. 실제 Wi-Fi PHY 자체를 낮춘 것은 아니다.
- E5는 재부팅·별도 이미지로 수행했으므로 live scene과 연결 상태까지 동일한 paired trace는 아니다. core1에는 net_tx도 있어서 AI 전용 코어가 아니다.
- 증거는 짧은 characterization이며 GT accuracy/DESR/energy를 측정하지 않았다. FOMO 오탐 여부와 무관하게 실행 비용을 기록했다.

## 원자료

- [Core0 E1–E4](results/causal-20260920T145209Z/REPORT.md)
- [Core1 pinning](results/pin-test-20260920T150021Z/REPORT.md)
- [실험 제어와 계측 정의](CAUSAL_PROTOCOL.md)
- [제한 제거 확인](results/causal-20260920T145209Z/network_cleanup.json)

다음 원인 분리 후보는 고정 JPEG trace에서 main의 QUIC 수신/ACK 처리 시간, net_tx의 packet build/encrypt/socket 및 QUIC mutex wait 시간을 따로 계측하는 것이다. 이를 측정하기 전에는 프레임워크나 새 스케줄러의 필요성을 결론으로 정하지 않는다.

## 종료 상태와 검증

네트워크 제한용 IFB/filter/clsact는 제거했고 기존 root qdisc를 유지했다. AI core0 배치의 causal-core0-v25로 복구했으며 profile0/probe0/mode2/policy1/30FPS 요청/6MiB 예산을 콘솔 ACK와 RAM telemetry로 확인했다. [복구 기록](results/causal-core0-v25/restoration.json). SDK 빌드, runtime control 경계값 host C 검사, Python compile, git diff whitespace 검사 통과. 각 정상 측정 구간에서 watchdog/panic은 관찰되지 않았다.
