# Baseline vs MOQT-Profile 비교 실험 프로토콜

목적: 2026-05-06에 수집한 기존(baseline, v1) 결과와 MOQT 프로파일 고도화 펌웨어(v2)의
성능을 동일 조건에서 비교하여, 표준화 논문의 정량 근거를 확보한다.

## 실험 매트릭스

| 축 | 값 |
|---|---|
| 펌웨어 | v1 (기존 데이터 `runs/`, `runs_stress/` 재사용) / v2 (MOQT 프로파일) |
| 전송 모드 | stream_only / datagram_only / hybrid (`app_config.h`의 `APP_TRANSPORT_MODE`) |
| 부하 | normal / stress (netem: delay 40ms, loss 1–3%, jitter 20ms, rate 8Mbit) |
| 반복 | 조건당 **최소 5회**, 회당 **90초** 캡처 |

- v1의 normal-load datagram_only는 세션 수립 실패로 데이터가 없음(알려진 결함, v2에서 수정).
  v2 결과만 제시하고 실패 원인은 논문에서 규범 조항의 근거로 사용.
- 반복 회차마다 디바이스를 리셋하여 핸드셰이크 포함 전체 사이클을 새로 측정.

## 실행 절차 (회당)

1. 서버: `AIOT_DGRAM_LOG=dgram_<mode>_r<N>.csv host_receiver/run_server.sh`
   (stress 조건이면 서버 측 인터페이스에 netem 적용, `experiments/capture_helpers/` 참고)
2. 모드 설정: `src/main/app_config.h`의 `APP_TRANSPORT_MODE` 수정 후 `idf.py build flash`
3. 캡처: `idf.py monitor | tee monitor.log` 90초
4. 파싱: `python3 telemetry_analyzer/parse_aiot_monitor_log.py monitor.log --out-dir runs_v2/<mode>_r<N>`
5. 수신측 파싱: `python3 telemetry_analyzer/parse_receiver_dgram_log.py host_receiver/dgram_<mode>_r<N>.csv --out-dir runs_v2/<mode>_r<N>`
   → 시퀀스 갭 기반 실손실, 재정렬/중복, OWDV(수신측 AoI 프록시) 산출

## 비교 분석

```bash
cd experiments/telemetry_analyzer
python3 compare_runs.py \
    v1-hybrid=runs/hybrid v2-hybrid=runs_v2/hybrid_r1 ... \
    --markdown out/comparison.md --csv out/comparison.csv
```

핵심 비교 지표 (compare_runs.py가 산출):
- **Freshness (e2e_us)** avg/p95/p99 — v2의 delivery timeout이 큐 적체분을 절단하는지
- **Queue wait** avg/p95 — v1 스트레스에서 평균 ~2초였음; v2 목표는 deadline(500ms) 이하
- **Datagrams lost (ACK-based)** — v2부터 ngtcp2 ACK 기반 실손실 (v1은 송신실패 근사)
- **Datagrams expired/evicted** — v2 신규; "선택적 셰딩" 동작의 직접 증거
- **Stream send fail** — 스트레스에서 제어 트래픽 보호(0 유지) 여부
- **RTT avg/p50/p95 (valid)** — `rtt_ms=0`(미측정 샘플) 제외 통계

## 자원 절약(Resource Conservation) 측정 — Saif & Matrawy (DoQ Datagram, IEEE 11062607) 방법론 차용

Stream 대비 Datagram의 자원 절약을 트랜잭션당 정량화한다 (해당 논문은 Pi Zero에서
메시지당 메모리 ~2.5kB, 시그널링 ~130B, 전력 59 vs 100.7mW 차이를 보고).
본 실험의 ESP32-S3 대응 측정:

| 자원 | 측정 방법 |
|---|---|
| 메모리 | telemetry.csv의 heap_free/heap_caps 추이 (교환 횟수로 정규화) |
| 시그널링 오버헤드 | 서버측 qlog(host_receiver/qlog_dir) 또는 tshark 캡처에서 STREAM/MAX_STREAMS 프레임 바이트 vs DATAGRAM 프레임 바이트 집계 (`capture_helpers/` 스크립트) |
| CPU | FreeRTOS run-time stats (`vTaskGetRunTimeStats`) 추가 필요 — 모드별 net_tx/main 태스크 CPU 시간 |
| 전력 | (선택) INA219/USB 파워미터로 모드별 평균 전력 |

네트워크 조건 스윕도 해당 논문과 정합: netem RTT 100–500ms(100ms 간격), loss 0–10% 스윕,
조건당 10회 반복이 이상적.

## 알려진 데이터 주의사항 (v1 기존 데이터)

1. **RTT 0값 오염**: v1 스트레스 캡처는 유효 RTT 샘플이 13–20%뿐이라 0 포함 평균(≈102ms)이
   실제(≈768ms)를 크게 왜곡한다. 워크숍 논문 표 2의 RTT는 이 문제를 포함하므로 KCI 원고에서는
   반드시 valid 통계로 교체할 것.
2. v1의 `datagrams_lost` 컬럼은 실손실이 아니라 송신 실패(큐 포화 포함) 카운터였다.
   v1↔v2 비교 시 이 컬럼은 의미가 다름을 명시할 것.
3. v1 hybrid의 `stream_window_available`은 항상 0(미계측 버그)이라 스케줄러가 상시 혼잡으로
   판단했다. v2와의 datagram 비율 차이 해석 시 참고.
