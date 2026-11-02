# ESP32 핸드오프와 soft migration 스케줄러

연구 질문: **단일 Wi-Fi 인터페이스를 가진 ESP32에서 AP 전환 중 QUIC 세션을 유지하면서 중요 이벤트의 전달과 최신 텔레메트리의 신선도를 어떻게 개선할 것인가?**

현재 ESP32-S3 + ngtcp2 + wolfSSL에서 실제 AP 전환을 시험한다. 이 구현은 QUIC connection migration이며, 동시 다중 경로 전송인 MP-QUIC 구현은 아니다. Linux MP-QUIC 보조 실험은 [../mobility](../mobility/README.md)에 남겨 둔다. 단일 무선 인터페이스의 재연결 구간을 없애는 것이 아니라 그 구간의 세션·큐·복구 전송을 관리한다.

## 구현과 비교군

| 모드 | 연결 복구 | 애플리케이션 큐 |
|---|---|---|
| `mode 0` | Wi-Fi 단절 후 QUIC/TLS/MQTT 재연결 | FIFO |
| `mode 1` | 같은 QUIC 연결의 경로 변경, 실패 시 재연결 | FIFO |
| `mode 2` | 같은 경로 변경 및 fallback | 중요 이벤트 우선, 일반 데이터 500ms 만료, 최신 우선, 복구 페이싱 |

상태는 normal → prepare/offline → validating → recovery → normal이다. 200ms마다 RSSI와 RTT를 읽고, RSSI < -70dBm 또는 RTT > 150ms가 3회 연속이면 prepare로 진입한다. 정상 표본 5회로 해제한다. prepare에서는 중요 이벤트만 전송한다. recovery 2초 동안 최대 100ms 간격, 그 외 20ms 간격으로 큐를 배출한다. 셀룰러의 지속적 고 RTT가 불필요한 prepare를 유발할 수 있으므로 이 임계값은 연구 가설이며 최적값이 아니다.

64개 객체 큐, 객체당 최대 160B. 중요 객체를 넣을 때 일반 객체를 먼저 축출하며 중요 객체만 가득 차면 손실 카운터를 올린다. 일반 텔레메트리는 QUIC DATAGRAM, 중요 이벤트는 MQTT QoS 0 over QUIC STREAM이다. QUIC 스트림 신뢰성은 **연결이 유지되는 동안** 적용된다. 재연결을 넘는 전달 보장은 아직 없고, application ACK/재전송은 후속 과제다.

연구용 입력은 약 100ms마다 생성되는 합성 텔레메트리이며 매 10번째가 중요 이벤트다. 기본 연구 빌드(`AIOT_MOBILITY_CAMERA=OFF`)는 카메라·AI를 초기화하지 않는다. Wi-Fi OTA 카메라 빌드(`AIOT_MOBILITY_CAMERA=ON`)는 실제 영상과 AI 추론을 함께 실행하므로, 아래 합성 입력 측정과 별도로 평가해야 한다. 따라서 이 결과는 AI 동시 실행 성능이 아니다. 원래 제품 진입점은 기본 빌드에 남아 있다.

Wi-Fi 이벤트 핸들러를 유지하고 IP 세대 변경을 추적한다. 새로운 로컬 UDP 소켓으로 기존 상대 서버에 `ngtcp2_conn_initiate_immediate_migration`을 호출한다. 경로 검증 6초, 단절 유예 20초, 초기 QUIC/MQTT 연결 15초 한도를 넘으면 fallback한다. 실제 경과 시간은 멈추지 않는다.

부수 수정: TX 태스크 스택을 내부 정적 메모리에 확보하고, STREAM 바이트를 PSRAM 링에 ACK까지 보관하여 호출자 버퍼 수명과 부분 쓰기 문제를 해결했다. 서버의 RETIRE_CONNECTION_ID 검사는 primary tuple의 CID 대신 **해당 패킷의 DCID**로 판정하도록 수정했다. [RFC 9000 §19.16](https://www.rfc-editor.org/rfc/rfc9000.html#section-19.16)의 동일 패킷 DCID 폐기 금지 검사는 유지한다.

## 현재 보드의 OTA 구성

보드는 후속 작업에서 2개 OTA 슬롯 구성으로 전환했다. 현재 무선 업데이트에는 **[OTA 전용 빌드/배포](../ota/README.md)**를 사용한다. 아래 factory 주소로 앱만 기록하는 절차는 이전 비-OTA 구성을 설명하며 현재 보드의 업데이트/복구 절차로 사용하지 않는다. OTA 백업 및 전체 복구 절차는 OTA 문서를 따른다.

## 빌드와 실행

ESP-IDF 5.2.2 환경, 이 저장소의 기존 sdkconfig 및 PSRAM 설정을 사용한다.

```bash
source /home/ryzen395/esp-idf/export.sh
cp sdkconfig sdkconfig.mobility
idf.py -B build_mobility -D SDKCONFIG=sdkconfig.mobility -D AIOT_MOBILITY_RESEARCH=ON build
bash experiments/handoff/build_receiver.sh
```

receiver 빌드는 기존 `host_receiver/picotls`의 정적 라이브러리를 사용한다. `build_handoff_receiver/picoquicdemo`가 수정된 서버다. 기존 배포 바이너리를 실행하면 수정이 반영되지 않는다.

현재 보드의 factory app은 0x10000, 크기 3MiB다. 기존 앱을 `results/device-backup/original-app.bin`에 백업한 후 **앱만** 기록했다. 이 오프셋은 다른 보드에서는 파티션 표를 먼저 확인한다.

```bash
python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
  write_flash 0x10000 build_mobility/aiot_quic.bin
python3 experiments/handoff/device_trial.py \
  --out experiments/handoff/results/my-trial --duration 75 --reboot \
  --server-host SERVER_REACHABLE_FROM_BOTH_APS --mode 2 --plan /tmp/handoff-plan.json
python3 experiments/handoff/analyze.py experiments/handoff/results/my-trial
```

`--reboot`는 이전 시험의 큐/카운터를 초기화한다. RAM 설정 명령은 부팅 후 3초에 전송하므로 NVS의 이전 서버 주소로 첫 접속이 시작될 수 있다. 따라서 30초 이상 준비하고 **session_ready가 실제 전환 전에 존재하는지** summary에서 확인한다. 모드 설정 이전의 초기 표본은 비교에서 제외해야 한다. 플랜은 자동 연결 완료를 기다리지 않는 고정 시간 스케줄이다.

플랜 예시(실제 비밀번호가 든 파일은 Git 밖, 권한 600으로 보관):

```json
[
  {"at_s":30,"commands":["set wifi.ssid AP_B","set wifi.pass PASSWORD_B","handoff"]},
  {"at_s":50,"commands":["set wifi.ssid AP_A","set wifi.pass PASSWORD_A","handoff"]}
]
```

`handoff`는 현재 RAM 설정으로 AP를 전환한다. 최종 펌웨어는 Wi-Fi 드라이버도 RAM 저장 모드로 설정하여 반복 전환에 따른 설정 플래시 쓰기를 피한다. `save`나 재부팅 없이 동작한다. iPhone에서는 2.4GHz 호환성이 켜져 있어야 한다. 서버 IP/UDP 포트는 두 AP에서 모두 도달 가능해야 하며 시험에서는 같은 서버 주소를 유지한다. 러너는 기본적으로 종료 시 자신이 띄운 서버를 종료한다. `--keep-server`를 주면 정상 측정 종료 후 서버를 남기고 manifest의 `receiver_pid`에 PID를 기록한다. 종료하려면 해당 PID가 이 receiver인지 확인한 후 `kill PID`를 실행한다. 이 모드는 분석용 `feed_snapshot.jsonl`을 종료 시 고정하고 `feed.jsonl`에는 실시간 수신을 계속 기록한다. 보드는 계속 실행하며 서버가 없으면 재연결을 시도한다.

## 결과 해석

- `serial.log`: 보드의 생성·큐 인계·Wi-Fi·경로 검증·세션 이벤트. `enqueue`는 수신 완료가 아니다.
- `feed.jsonl`: 수신한 실제 payload의 seq와 gen_us. DATAGRAM 헤더 seq와 payload seq는 별개다.
- `packets.log`: 경로/CID/서버 종료 원인을 확인하는 패킷 로그.
- `manifest.json`: 명령, 모드, 서버 해시, 로컬 펌웨어 해시. 로컬 해시만으로 실제 플래시 버전을 증명하지 않으므로 플래시/부팅 기록도 보관한다.
- `summary.json`: Wi-Fi 중단, 경로 검증, 세션 유지, 생성/수신 대조. 세션 유지 판정에는 전환 전 MQTT 준비, 같은 세션 및 receiver CID, 전환 후 수신, 새 세션/fallback 없음이 필요하다.

보드와 서버 시계를 직접 빼서 one-way latency라고 부르지 않는다. receiver generation-boundary gap은 전환 전/후 생성 객체의 **수신 시각 간격**이며 생성 주기와 큐 효과를 포함한다. 전환 전에 생성된 객체가 전환 후 늦게 도착하면 이 값은 실제 단절보다 짧아질 수 있으므로 서비스 중단 시간으로 해석하지 않는다. `first_outage_critical_enqueue_age_ms`는 단절 이후 처음 생성된 중요 객체가 로컬 전송 계층에 인계될 때의 나이이며 수신 지연은 아니다. 중요 이벤트 전달률은 첫 session_ready 이후부터 기록 종료 2초 전까지 생성된 `(seq, gen_us)`를 실제 수신과 대조한다. 보드 재부팅 전 버퍼 로그와 잘린 생성 로그는 제외한다. 원시 파일은 `.gitignore` 대상이며 비밀번호를 저장소에 넣지 않는다.

현재 시험은 정지된 보드에서 명령으로 발생시킨 AP 전환이다. 실제 이동 궤적, RF 감쇠, 반복 통계, 긴 단절, AI 부하를 추가해야 스케줄러 효과를 주장할 수 있다. 최근 결과는 [RESULTS.md](RESULTS.md)에 기록한다.

## 검증과 복구

```bash
python3 -m unittest discover -s experiments/handoff -p 'test_*.py'
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I src/components/dl_qads/include -I src/components/transport \
  experiments/handoff/test_scheduler.c src/components/dl_qads/mobility_scheduler.c \
  -o /tmp/aiot_handoff_test
/tmp/aiot_handoff_test
```

원래 앱 복원은 동일 보드에서 아래 앱 백업을 기록한다. NVS와 파티션은 보존한다.

```bash
python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
  write_flash 0x10000 experiments/handoff/results/device-backup/original-app.bin
```
