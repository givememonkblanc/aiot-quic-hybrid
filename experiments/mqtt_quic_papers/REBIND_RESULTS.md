# Loopback NAT 재바인딩 시험

실제 MQTT QoS1 / QUIC 단일 STREAM을 사용한다. 30개/초 × 6초, 256KiB payload, 6MiB application queue, 2초 deadline 및 종료 drain이다.
동일 중계를 거치는 정상 대조군, 3초 양방향 UDP blackhole 후 upstream source port 변경, 그리고 복구 20ms 뒤 양쪽 client의 저자 migrate() API 호출을 비교한다. 이는 AP 핸드오프/ESP32 시험이 아니다.

| 방식 | 큐 정책 | 실행 완료/시도 | 수신/생성 | deadline 충족 | 큐 최고 MiB | 검증된 복구/시도 | 새 입력 수신까지 평균 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| relay control | fifo | 2/2 | 360/360 | 100.0% | 0.25 | 해당 없음 | 미복구/해당 없음 |
| relay control | deadline | 2/2 | 360/360 | 100.0% | 0.25 | 해당 없음 | 미복구/해당 없음 |
| relay control | latest | 2/2 | 360/360 | 100.0% | 0.25 | 해당 없음 | 미복구/해당 없음 |
| relay control | amr_latest_queue_only | 2/2 | 360/360 | 100.0% | 0.25 | 해당 없음 | 미복구/해당 없음 |
| passive rebind | fifo | 2/2 | 60/360 | 16.7% | 6.00 | 0/2 | 미복구/해당 없음 |
| passive rebind | deadline | 2/2 | 60/360 | 16.7% | 6.00 | 0/2 | 미복구/해당 없음 |
| passive rebind | latest | 2/2 | 60/360 | 16.7% | 1.25 | 0/2 | 미복구/해당 없음 |
| passive rebind | amr_latest_queue_only | 2/2 | 60/360 | 16.7% | 0.25 | 0/2 | 미복구/해당 없음 |
| active migrate() | fifo | 2/2 | 207/360 | 44.2% | 6.00 | 2/2 | 379.16 |
| active migrate() | deadline | 2/2 | 179/360 | 49.2% | 6.00 | 2/2 | 54.70 |
| active migrate() | latest | 2/2 | 178/360 | 48.9% | 1.25 | 2/2 | 196.37 |
| active migrate() | amr_latest_queue_only | 2/2 | 160/360 | 43.9% | 0.25 | 2/2 | 383.32 |

## 복구 판정

단순히 QUIC 객체가 남아 있거나 publish가 성공했다고 복구로 세지 않는다. 다음 조건을 모두 확인한다.

1. publisher와 subscriber 각각 시작/종료 Quinn stable_id 동일, 종료 원인 없음.
2. 두 client 모두 PATH_CHALLENGE 수신 및 PATH_RESPONSE 송신 카운터 증가.
3. network 복구 시각 이후 생성한 실제 payload가 subscriber callback에 도달.

raw.json의 transport_evidence/migration_events, relay.json의 단절·원래/새 source port·forward/drop 카운터를 보존한다.

지연 기준은 fixture의 명목 network 복구 시각이며 실제 중계 rebind 시각은 relay 로그에 별도로 있다. 루프 타이머 오차를 포함한다. 복구된 실행만의 지연 평균 옆에 복구 성공 횟수를 함께 표시한다.

passive rebind에서 수동적으로 수신만 하는 subscriber는 새 NAT tuple을 서버에 알릴 트래픽이 없을 수 있다. 8초 관측 안에 미복구인 것은 영구 복구 불능이라는 뜻이 아니다. 기본 MQTT keepalive/재전송 timeout 이후는 이번 관측 범위 밖이다.

active migrate()는 publisher/subscriber endpoint를 새 로컬 UDP socket에 rebind한다. 논문의 저자 라이브러리 API를 호출한 호스트 실험이다. ESP32의 ngtcp2 구현이나 Wi-Fi link-up 감지에 이 효과가 그대로 적용됐다고 주장하지 않는다.

중계는 kernel UDP buffer와 Python CPU 비용을 추가한다. 그래서 udp-pass 대조군을 포함한다. 패킷 손실 확률·RTT·전력·실제 RF 이동은 통제하지 않았다. 작은 반복 수이므로 수십 ms 차이의 통계적 우열은 주장하지 않는다.
