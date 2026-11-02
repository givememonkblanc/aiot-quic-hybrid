# AMR 스케줄러 원본 수집 및 ESP32 비교

분석일: 2026-09-20. **AMR의 경로 선택 정책 8종을 모두 가져왔으나, 실제 구현은 5종이고 ECF·BLEST·ToF는 placeholder다.** 현재 ESP32 정책은 단일 Wi-Fi 연결의 migration 및 영상 큐 제어다. AMR의 두 경로 선택과 같은 기능으로 취급하면 안 된다.

## 가져온 범위와 검증

- `reference/src/client/scheduler/`: 8종 정책, dispatcher, 공통 함수와 PQI 파라미터 원본.
- `reference/src/client/`: 정책 호출, 경로 건강 판정, STREAM admission, 옵션·세션·transport parameter를 확인할 연결 코드.
- `source_manifest.json`: 원본 `/home/ryzen395/mpquic`의 Git HEAD와 20개 파일별 SHA-256. Git HEAD만이 아니라 당시 working tree를 복사했다. 원본 저장소는 수정하지 않았다.
- `results/analysis.json`: 정지 시험 80회 및 AMR 프로젝트의 메타데이터가 있는 20개 로그 분석. 원본 영상이나 인증 파일은 포함하지 않았다.
- `results/decision_probe.csv`: 가져온 C 코드에 동일한 두 경로 상태를 넣은 34개 결정. 원래 `path_is_healthy()`도 사용한다. 외부 로깅만 no-op 처리했다. ASan/UBSan 및 assertion 통과. 전송 성능 시뮬레이션이 아니다.

이 원본은 비교용으로 보존하며 직접 ESP32 펌웨어에 링크하지 않는다. 이후 단일 경로용 이식 구현과 실기 시험은 [PORT_RESULTS.md](PORT_RESULTS.md)에 별도 기록한다. AMR 실행 설정이 가리키는 것은 C/picoquic 프로젝트다. 별도 `mp-quic-go`의 Go 정책과 결과는 이번 AMR 구현에 섞지 않았다. 과거 배포 바이너리와 현재 소스의 해시 연결은 확보하지 못했으므로, 현재 구현 검증과 과거 측정 결과를 구분한다.

## 8종 구현 비교

| 정책 | 실제 입력·판정 | 큐/메모리 제어 | 확인한 제약 |
|---|---|---|---|
| PQI | RTT·loss·BW EWMA, 두 경로 min-max 정규화. 가중치 0.45/0.35/0.20. grade 및 점수로 전환 | 정책 자체에는 없음 | 전환 650, 복귀 450, 점수 차 120. 기본 debounce 2초, backup 유지 3초, 반복 전환 시 최대 60초. 절대 grade가 상대 점수의 흔들림을 억제 |
| RSSI-aware | Wi-Fi RSSI와 경로 건강. ≤−75dBm에서 backup, >−67dBm에서 복귀 조건 | 정책 자체에는 없음 | 전환 상태 저장 누락, UNKNOWN 처리 및 유지 시간 의미 문제 아래 참조 |
| minRTT | verified·non-demoted 경로 중 SRTT 최소 | 없음 | 재전송/무응답 기반 health 검사를 사용하지 않아, RTT가 오래된 죽은 경로를 계속 고를 수 있음 |
| RR | verified·non-demoted 경로를 순환 | 없음 | 파일 static counter. 비대칭 경로에도 균등 선택. 다음 호출은 새 경로를 고르므로 reset되지 않은 상태를 고려해야 함 |
| SPQUIC 이름의 정책 | 살아 있는 primary IP 우선, 실패 시 다른 live 경로, 마지막으로 verified fallback | 없음 | 단일 경로 선택 정책이나 현재 공통 transport는 multipath 활성화. 독립적인 SP-QUIC 프로토콜 기준선으로 단정 불가 |
| ECF | `primary_i` 반환, `TODO_ecf` | 없음 | 완료 시간 추정 및 fast-path wait 미구현 |
| BLEST | `primary_i` 반환, `TODO_blest` | 없음 | blocking 추정 미구현 |
| ToF | `primary_i` 반환, `TODO_tof` | 없음 | in-flight 시간 추정 미구현 |

원본 경로: `reference/src/client/scheduler/sched_*.c`. 문서와 주석보다 실제 함수 본문·호출부·로그를 판정 근거로 사용했다. 특히 dispatcher의 enum 확장 TODO는 낡았지만 ECF/BLEST/ToF 함수 본문의 TODO는 실제 미구현이다.

## 현재 코드에서 재현한 문제

1. **RSSI 선택 후 상태가 저장되지 않는다.** `sched_rssi_aware_select()`는 `pqi_last_choice`와 `pqi_last_switch_us`를 읽지만 변경하지 않는다. `client_loop.c:select_send_path()`에도 commit이 없다. `update_pqi_choice_state()` 호출은 PQI 모듈에만 있다. 원본 probe에서 −80dBm으로 backup을 선택한 뒤 −70dBm에서 바로 primary로 돌아왔다. 이 코드 그대로를 히스테리시스 기준선으로 쓰면 안 된다.
2. **RSSI UNKNOWN이 나쁜 링크로 처리된다.** `SCHED_WIFI_UNKNOWN`의 enum 값이 BAD보다 커 `grade >= SCHED_WIFI_BAD` 조건에 포함된다. 두 경로가 정상이어도 RSSI를 모르면 backup을 선택한다.
3. **1초 GOOD 유지와 backup 체류 시간이 다르다.** 코드는 `now - last_switch`를 사용한다. 상태 저장을 고쳐도 GOOD 상태가 연속 1초였는지 측정하지 않는다. 오랫동안 backup에 있었다면 첫 GOOD 표본에서 즉시 복귀한다.
4. **minRTT/RR의 health gate가 PQI/RSSI/SP selector와 다르다.** probe의 primary는 verified 상태지만 재전송 3회·in-flight 잔량이 있다. minRTT는 primary, SP selector는 backup을 골랐다. 동일 health gate로 통일하면 원본과 달라지므로 별도 ablation으로 표시해야 한다.
5. **PQI 점수 1000과 unusable이 겹친다.** `choose_path_via_pqi()`는 grade가 아니라 `score < 1000`으로 usable을 판정한다. 두 경로 정규화에서 grade 1 경로도 정확히 1000점이 될 수 있다. probe의 RTT/loss 열화 조건은 `degrade_failover`가 아닌 `backup_only`로 빠진다. 추후 이식 시 grade와 점수 역할을 분리할 검토가 필요하다.
6. **SP 기준선의 프로토콜 분리가 안 돼 있다.** `client_uploader.c`는 정책 모드와 무관하게 `initial_max_path_id = 3`을 설정한다. 현재 이름의 `spquic` 결과를 순수 SP-QUIC 대 MP-QUIC 비교로 인용하지 않는다.

원본의 문제를 숨기기 위해 가져온 파일을 수정하지 않았다. 수정 정책은 별도 이름과 동일 trace 검증이 필요하다.

## AMR에서도 메모리를 제어한 것은 별도의 admission 계층

`client_loop.c`와 `client_paths.c`에는 경로 선택과 별개로 다음 동작이 있다.

- 카메라는 최신 프레임으로 덮어쓰며, 각 media STREAM의 대기 바이트를 센다.
- `MPQUIC_TX_QUEUE_FRAMES=1` 기본값에서는 해당 STREAM의 큐가 비어야 새 프레임을 넣는다.
- 경로 in-flight 상한은 `BW × MPQUIC_BP_TARGET_MS / 1000`, 목표 200ms, 최소 64KiB, BW 미확인 시 128KiB다.
- depth/RGB STREAM에는 기본 even priority 8과 애플리케이션 측 교대 admission을 적용한다.

즉, AMR에서도 경로 선택만으로 무한 STREAM 큐가 해결된 것은 아니다. 경로 선택과 admission의 효과를 분리해서 측정해야 한다. ESP32 현재의 한 프레임 admission·만료·감지 결과 우선 처리도 이 계층에 해당한다. 메모리 압박을 같게 하려면 모든 정책에 같은 입력과 **동일한 허용 메모리**를 주어야 한다. 효율적인 정책까지 같은 사용률을 유지하도록 강제로 채우면 적체 억제 효과가 가려진다.

## 과거 정지 시험 재분석

자료: `/home/ryzen395/mpquic/results/stationary_20260720/master_summary.csv`, 정책별 10회. **80개 원시 `server_rx.csv`에서 frame 수, byte 수, goodput을 재계산하여 요약과 대조했다.** 아래는 기존 요약의 소수 둘째 자리 수치로 계산한 평균과 표본 표준편차다.

| 정책 | 수신 goodput 평균 ± SD (Mbps) | 로그상 경로 변경 평균/회 | 판정 |
|---|---:|---:|---|
| PQI | 10.832 ± 0.104 | 0.0 | 정지 조건의 구현 정책 |
| RSSI | 10.848 ± 0.057 | 0.0 | 정지 조건의 구현 정책 |
| minRTT | 10.861 ± 0.076 | 130.9 | 경로 선택 변경이 잦음 |
| SP selector | 10.809 ± 0.103 | 0.0 | multipath 공통 스택 위의 선택 정책 |
| RR | 10.835 ± 0.087 | 636.5 | 의도한 순환 선택; 모두 원치 않는 핸드오프로 세면 안 됨 |
| ECF 이름 | 10.818 ± 0.071 | 0.0 | 10/10 로그 `TODO_ecf`: 알고리즘 비교 제외 |
| BLEST 이름 | 10.833 ± 0.066 | 0.0 | 10/10 로그 `TODO_blest`: 알고리즘 비교 제외 |
| ToF 이름 | 10.854 ± 0.079 | 0.0 | 10/10 로그 `TODO_tof`: 알고리즘 비교 제외 |

모든 client exit code는 1이지만, 80/80 로그에 설정된 duration 경과로 종료한 기록이 있다. 이를 80회 오류 종료로 세지 않았다. RGB 약 30FPS와 depth 약 20FPS를 합친 frame 수이므로 단일 카메라 50FPS를 의미하지 않는다. 평균값의 작은 차이를 우열이나 유의성으로 주장하지 않는다. 정지 시험에는 ESP32 RAM, 메모리 압박, 실제 AP handoff 검증이 없다.

## AMR 이동 프로젝트 로그의 비교 가능성

`slam-mpquic-exp/results`에서 client meta와 event가 있는 20개 기록을 확인했다: PQI 13개, RSSI 3개, RR 4개. 정지·센서 점검도 포함돼 모두 주행 실험은 아니다. 잘린 event 행은 제외하고 개수를 `malformed_events`로 기록했다. 경로 index 0/1을 항상 Wi-Fi/LTE 역할로 확정하지 않고 원자료 그대로 보존했다.

- 프로젝트의 기존 `compare_schedulers.py --all`을 재실행한 결과, 설정 mode 이름에 맞는 `pqi_1..4` 중 `pqi_1..3`은 ODOM 좌표라 제외됐다.
- 남은 것은 `pqi_4`의 단일 정책 위치 프로파일이다. 기존 `--all`은 `soak_*`와 진단 이름의 실행을 포함하지 않는다.
- `pqi_2`는 event 기준 `primary_only` 약 88.9%다. 두 경로 선택 가능성이 적었던 구간을 성능 우열 근거로 사용하지 않는다.
- 일부 기록에는 서버 시각 앵커가 없다. 송신 바이트로 대체한 값을 수신 goodput처럼 섞어 쓰지 않는다.

따라서 확보된 자료로 **8종 이동성 성능 순위를 만들 수 없다.** 실행 흔적과 비교 가능한 동일 조건의 반복 시험은 별개다. 위치 분석 결과와 제외 사유는 `results/position_analysis.log`, `position_comparison.csv`에 있다.

## 현재 ESP32와의 대응 및 다음 비교 설계

현재 ESP32 v19의 짧은 실측은 1024×1024/q10, 목표 30FPS, payload 예산 6MiB, 정책별 35초다. 실제 입력은 장면과 CPU 부하에 따라 달랐다. `../person_detection/results/pressure-v19-run01/` 참조.

| 지표 | FIFO mode 1 | 현재 adaptive mode 2 |
|---|---:|---:|
| PSRAM 최고 사용률 | 97.15% | 30.60% |
| queue payload 최고점 | 5,712,472B | 166,103B |
| 실제 캡처 FPS | 5.23 | 6.65 |
| 완성 영상 수신 FPS 평균 | 0.67 | 0.58 |
| AI 결과 수신 FPS 평균 | 0.01 | 0.59 |

두 모드 모두 시험 중 재부팅 없이 완료했지만 MQTT 타임아웃/재연결은 남아 있다. FPS는 10초 창을 phase 10초 이후 표본에서 평균한 값이며, 메모리는 5초 주기다. 정책 순서를 교차하지 않은 순차 live 장면 시험이라 일반화할 수 없다. **적응형의 메모리·AI 개선은 관찰됐으나 영상 처리량 승리는 아니다.** 30FPS는 요청값이며 실제 30FPS 수신이 아니다.

| 가져올 로직 | ESP32 적용 위치 | 비교 조건 |
|---|---|---|
| RSSI 경고·히스테리시스 | handoff prepare/recovery 상태 | 상태 저장·UNKNOWN·연속 정상 시간 수정 후, 원본과 수정본을 구분 |
| PQI의 EWMA·절대 열화 gate·debounce | 이동성 상태 판정 | 단일 경로에서는 두 경로 min-max 점수로 우열을 만들지 않음. loss/BW 실제 계측부터 확보 |
| AMR STREAM별 admission | 영상/감지/제어 큐 admission | 동일 6MiB 예산과 입력, 사용률·최신성·완성 frame goodput 동시 측정 |
| ECF 완료 시간 아이디어 | JPEG 전체의 deadline 충족 여부 판단 | 기존 AMR 구현이 없으므로 새 `ECF-inspired` 정책으로 명명하고 공식 ECF 재현과 구분 |
| ToF 아이디어 | 미진행 시간·in-flight 연령 기반 정체 감지 | 현재 RTT/cwnd/inflight 계측에 ACK 진행 시각 추가 필요. 기존 AMR ToF 성능으로 주장 불가 |
| BLEST | 신뢰성·순서 보장 트래픽의 blocking 제어 | 현재 영상 DATAGRAM은 전송 계층의 in-order STREAM이 아님. 그대로 이식해 원래 BLEST라고 비교하지 않음 |
| minRTT/RR 경로 선택 | 동시 사용 가능한 복수 경로가 있을 때 | 현재 ESP32 단일 Wi-Fi 환경에서는 경로 선택 비교가 성립하지 않음 |

실험은 두 축으로 분리한다. 먼저 FIFO / latest-frame admission / 현재 QUIC 지표 기반 admission / 수정한 RSSI·진행 정체 기반 정책을 **같은 transport·해상도·입력·메모리 예산**에서 비교한다. 다음으로 같은 큐 정책에 reconnect와 QUIC migration을 바꾸어 이동성 복구 효과를 측정한다. AMR의 두 경로 정책은 두 경로가 실제 동시에 사용 가능한 플랫폼에서 별도 비교한다.

기록할 지표는 capture·admission·complete-frame FPS, goodput, frame age p50/p95, handoff 수신 공백, drop 사유, queue peak, 내부 heap/PSRAM 최소 여유와 최대 연속 블록, AI 최신성, 복구 시간, 재연결·재부팅 수다. 15/30FPS 요청을 유지하고 순서를 교차하며 반복해야 한다. 실제 캡처 속도가 다른 경우 고정된 frame trace replay를 별도 시험으로 추가하고 live 카메라 결과와 섞지 않는다. 메모리 관측값과 transport counter만으로 논문 수준의 handoff 개선을 주장하지 않는다.

## 재실행

```bash
python3 experiments/amr_comparison/analyze.py
python3 experiments/amr_comparison/run_probe.py
```

분석은 기존 로컬 자료를 읽고 이 디렉터리의 `results/`만 갱신한다. probe에는 원본 picoquic 헤더가 필요하며 `--picoquic PATH`로 지정할 수 있다. 로봇 구동, 네트워크 장애 주입, 원격 배포를 수행하지 않는다.
