# ESP32 RAM 시계열 측정

카메라 연구 빌드는 기존 esp_timer 태스크에서 1초 주기로 내부 8-bit heap, PSRAM, 내부 DMA heap을 측정한다. 약 5초마다 연결 가능한 경우 MQTT/QUIC STREAM의 `aiot/diagnostics/ram`으로 최신 표본을 보낸다. AI 추론과 OTA가 메인 루프를 점유해도 샘플링 타이머는 별도로 동작한다. 타이머 스케줄 지연은 가능하다.

- `*_free`: 현재 할당 가능한 heap 바이트.
- `*_min`: 부팅 이후 heap 최저 여유량. 여러 heap의 최저값 합계이므로 한 순간의 정확한 사용량 최고치와 같지는 않다.
- `*_largest`: 최대 연속 할당 가능 블록. 여유량이 커도 이 값이 작으면 큰 할당이 실패할 수 있다.
- `*_total`: 해당 capability heap의 할당량 + 여유량. 칩 전체 물리 RAM 크기가 아니며 정적 데이터·예약 영역은 제외된다.
- DMA는 내부 RAM과 겹친다. 두 값을 합산하지 않는다.
- `main_stack_min`: 메인 태스크 stack high-water mark(ESP-IDF 바이트 단위).
- `seq`, `uptime_us`, QUIC `cid`로 표본 누락과 재연결/재부팅을 구분한다.
- `state`, `frames`, `ai_us`로 이동성 상태·프레임·추론 부하를 함께 기록한다.

서버 수신 시간이 CSV에 추가된다. 연결 단절/OTA 중에는 서버 전송이 멈추며 모든 1초 표본을 저장하거나 재전송하지 않는다. 부팅 이후 최저 여유량은 재연결 후에도 보고된다. CSV는 이어쓰기이며 `latest.json`은 마지막 수신값이므로 수신 시각으로 오래된 값을 구별해야 한다.

```bash
python3 experiments/memory/collect.py \
  --feed experiments/handoff/results/cross-ap-final-mode2-03/feed.jsonl \
  --output experiments/memory/results
```

`results/ram.csv`에 시계열, `results/latest.json`에 최신 값이 저장된다. 프로세스는 현재 서버 세션에서 실행하며 서버 재부팅 시 자동 시작되지는 않는다. feed 파일 교체 시 재시작이 필요하다. 기존 대시보드와 인증 설정은 변경하지 않는다.

카메라 v6는 320×240 JPEG, PSRAM 32KiB 복사 버퍼, AI용 1/2 JPEG 디코딩(160×120)을 사용한다. 모델 입력/가중치는 그대로다. 32KiB 초과 JPEG는 잘라 보내지 않고 프레임 실패로 처리한다.
