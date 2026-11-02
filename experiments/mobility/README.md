# 이동성 환경 MP-QUIC 측정

이 디렉터리는 기존 AIoT/MQTT 실험과 독립적인 **Linux picoquic 양단 측정 환경**이다.
실제 QUIC 패킷을 독립된 두 veth 경로로 전송하고, Linux netem으로 이동에 따른
경로 품질 악화·단절·복구를 재현한다. 무선 이동 자체를 시뮬레이션하지는 않는다.
기존 ESP32 펌웨어의 stream/datagram hybrid와 MP-QUIC는 서로 다른 기능이다.

## 실행

필요 도구: Linux, Python 3 표준 라이브러리, iproute2 (`ip`, `tc`, `ss`), OpenSSL,
`setpriv`, 저장소의 `host_receiver/picoquic/picoquicdemo` 바이너리.
네임스페이스와 qdisc 생성에는 root 권한이 필요하다. 일반 사용자로 실행할 경우
`sudo -n`을 사용한다. 권한이 없다면 로컬 터미널에서 `sudo -v` 후 실행한다.

```bash
# 단일 조건: 기본 32MiB, 경로 변화 시점 5/10/15초
python3 experiments/mobility/run.py \
  --mode multipath --scenario handover \
  --out experiments/mobility/results/mp-handover-r1

# 짧은 기능 검증: 4MiB, 변화 시점 1/2/3초
python3 experiments/mobility/run.py \
  --mode multipath --scenario handover --bytes 4194304 --time-scale .2 \
  --out experiments/mobility/results/smoke-01

# 조건별 5회, 총 30회 순차 실행; 각 반복에서 조건/모드 순서를 섞는다
python3 experiments/mobility/matrix.py \
  --out experiments/mobility/results/matrix-01 --repeats 5

# 기존 결과 재분석
python3 experiments/mobility/analyze.py experiments/mobility/results/mp-handover-r1
python3 experiments/mobility/matrix.py --analyze-only \
  --out experiments/mobility/results/matrix-01

# 비교 그래프 PNG/PDF 생성 (선택: matplotlib 필요)
python3 experiments/mobility/plot.py experiments/mobility/results/matrix-01

# 분석기 회귀 테스트: 장비/root 불필요
python3 -m unittest discover -s experiments/mobility -p 'test_*.py' -v
```

출력 폴더는 매 실행 새 경로여야 한다. 기존 측정 결과를 덮어쓰지 않는다.
실패/타임아웃 로그는 보존하고 실행기는 0 이외 종료 코드를 반환한다.

## 토폴로지와 비교 조건

```text
client namespace                     server namespace
10.201.0.1:path0 ----- veth ----- path0:10.201.0.2
10.202.0.1:path1 ----- veth ----- path1:10.202.0.2
```

| 항목 | 설정 |
|---|---|
| primary | 편도 10ms, 각 방향 4Mbps, 초기 손실 0% |
| secondary | 편도 30ms, 각 방향 2Mbps, 초기 손실 0% |
| single | primary에 고정된 단일 경로 QUIC |
| multipath | 양단 `-M`, secondary 주소/인터페이스를 `-A`로 추가 |
| 전송 | hq-interop 합성 파일 다운로드, 기본 32MiB, 디스크 저장 없음 |
| 혼잡 제어 | 양단 CUBIC, 단일 실행에서 `--cc bbr` 선택 가능 |
| 반복 | 기본 조건별 5회, 실행 순서 seed 기록 |
| 연결 | 매 실행 새 프로세스·TLS 연결·티켓 저장소 |
| 인증 | 실행별 생성한 테스트 인증서를 클라이언트가 신뢰하도록 설정 |

두 경로는 서로 다른 주소/인터페이스를 사용한다. 단순히 같은 주소에서 포트만
바꾼 것을 독립 경로 측정으로 취급하지 않는다. `single`은 동일한 토폴로지에서
보조 경로를 사용하지 않는 기준선이며, **QUIC migration 기준선은 아니다**.

| 시나리오 | 이벤트 (기본 시각, 클라이언트 시작 기준) |
|---|---|
| static | 변화 없음 |
| outage | 5초 primary 양방향 손실 100%, 10초 복구 |
| handover | 5초 primary 편도 80ms·손실 2%·1Mbps, 10초 단절, 15초 복구 |

`--time-scale .2`는 이벤트 시각만 1/5로 줄인다. 지연·속도·손실은 동일하다.
`handover`는 **이미 확보한 보조 경로가 있는 상황의 품질 변화/블랙홀 모델**이다.
AP 재연결, 스캔, DHCP, 새로운 경로의 지연된 발견은 포함하지 않는다.
100% 손실은 링크를 내리는 대신 블랙홀을 만들어 QUIC의 장애 감지를 측정한다.
`tc qdisc replace`는 해당 경로의 대기 패킷을 버릴 수 있으므로 그 영향도 포함된다.

## 결과와 검증

실행별:

- `manifest.json`: 바이너리 SHA-256, Git HEAD/작업 상태, 커널, 전체 명령,
  토폴로지, 실제 이벤트 시각, 오류/타임아웃 여부.
- `client.log`, `server.log`, 양단 `*_qlog/`: 원시 근거.
- `*_qdisc.json`: 종료 시 qdisc 통계.
- `progress.csv`: 수신 STREAM 고유 바이트와 연속 오프셋 진전 기록.
- `summary.json`: 검증 판정과 경로별/전체 측정치.

행렬 실행은 `schedule.json`, `aggregate.json`, `aggregate.csv`, `report.md`를 추가한다.
모든 실패를 유효/전체 회수에 남기고, 수치 평균과 표본 표준편차는 유효 실행만 사용한다.
seed는 **실행 순서만** 고정한다. 커널 손실 난수와 QUIC 난수는 고정하지 않는다.

유효 실행의 조건:

1. 클라이언트 정상 종료와 STREAM FIN까지 빈틈 없는 전체 바이트 수신.
2. 요청한 파일 크기와 수신 고유 바이트 일치.
3. 모든 시나리오 이벤트 적용 및 마지막 이벤트 이후 수신 진전.
4. MP 모드는 명시적 협상 성공 **및 두 path ID에서 고유 STREAM 바이트 수신**.

`-M`만 켰거나 연결만 성공한 경우는 MP 측정 성공으로 인정하지 않는다.

| 지표 | 정의와 한계 |
|---|---|
| goodput | 수신 STREAM 오프셋 합집합 바이트 × 8 / 연결 시작부터 마지막 연속 바이트까지 시간 |
| completion | 연결 시작 → 마지막 연속 STREAM 바이트, 핸드셰이크 포함 |
| max progress gap | 연속 STREAM 오프셋이 진전한 두 시각의 최대 간격; 순서 역전으로 인한 대기도 포함 |
| next progress after event | qdisc 변경 완료 → 다음 연속 수신 진전; **handover 완료 시간과 같지 않음** |
| straddling gap | 이벤트 전 마지막 진전 → 이벤트 후 첫 진전 |
| path bytes | 해당 경로로 먼저 도착한 고유 STREAM 바이트; 재전송 중복 제외 |
| path RTT | qlog에 기록된 smoothed_rtt 업데이트의 표본 p50/p95; 시간 가중 평균이 아님 |

Linux picoquic의 `CLOCK_MONOTONIC`을 Python `time.monotonic()`과 맞춘다.
UTC 시각과 혼합하지 않으며 이벤트 적용 시작/끝을 모두 기록한다. qdisc 양방향
변경은 원자적이지 않으므로 적용 소요 시간도 결과에 남긴다.
분석기는 저장소 picoquic의 draft-00 qlog 배열 형식을 대상으로 한다.
수신 오프셋으로 재조립 진전을 추정하며 실제 애플리케이션 콜백 시각은 계측하지 않는다.

## 연구 범위와 다음 단계

이 측정으로 답할 수 있는 질문은 “두 독립 경로를 미리 확보했을 때, 한 경로의
악화/단절에 대해 picoquic MP가 primary 고정 QUIC보다 전송 연속성을 유지하는가?”이다.
MP의 합산 대역폭이 더 크므로 처리량 증가만으로 이동성 이득을 주장하지 않는다.

실제 이동성 연구를 완성하려면 다음 확장이 필요하다.

- 단일 경로 QUIC **migration** 기준선과 경로 추가/제거 시점 제어.
- Linux 이동 단말의 Wi-Fi+셀룰러 또는 AP 전환 실측, RSSI/association/IP 이벤트 기록.
- 고정 전송량뿐 아니라 고정 시간·고정 생성률의 상향 IoT 트래픽과 메시지 deadline.
- 장애 전/중/후 구간의 전달률·지연, 충분한 반복/신뢰구간, 경로 스케줄러 비교.
- 장비가 ESP32라면 다중 인터페이스 가용성과 클라이언트 MP 스택 포팅 가능성 확인.

기존 AI 모델/QUIC 미커밋 수정과 과거 실험은 유지한다. 현재 실행기는 펌웨어를
플래시하거나 기존 수신 서버를 중지하지 않는다. 네임스페이스 이름은 실행별로
고유하며 정상 종료·예외·SIGINT/SIGTERM에서 생성한 자원만 정리한다.
SIGKILL/전원 차단에는 정리가 불가능하므로 남은 `mpq-*` 네임스페이스를 확인한다.

## 프로토콜 버전

저장소 `picoquic.h`에는 multipath draft-20 코드 포인트 주석이 있다.
IETF 문서는 별도 발전하므로 현재 최신 명세와의 호환성을 이 실험만으로 주장하지 않는다.
실행에 사용한 바이너리 해시와 버전 출력이 실제 측정 구현 식별자다.

- IETF 문서: https://datatracker.ietf.org/doc/draft-ietf-quic-multipath/
- 구현: https://github.com/private-octopus/picoquic
- 로컬 API: `host_receiver/picoquic/picoquic/picoquic.h`
- 로컬 경로 설정: `host_receiver/picoquic/picoquicfirst/picoquicdemo.c`
