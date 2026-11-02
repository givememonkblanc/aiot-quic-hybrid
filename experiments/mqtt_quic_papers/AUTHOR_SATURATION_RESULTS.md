# 저자 구현 기반 비교 결과

동일 Rust MQTT broker/client 구현에서 TCP+TLS와 QUIC 스트림 배치만 변경했다. 인증서 검증을 켜고 client/broker의 정책을 맞췄다. loopback 결과이며 ESP32·손실·핸드오프 우위는 평가하지 않는다.

| 메시지 B | QoS | 방식 | 성공/실행 | unique 수신/송신 | 실행별 p95 평균 ms | p99 평균 ms | broker CPU ms | broker peak RSS MiB |
|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 32768 | 0 | control-only | 3/3 | 380111/380111 | 117.446 | 124.698 | 10200.000 | 119.944 |
| 32768 | 0 | per-publish | 3/3 | 301315/368149 | 559.972 | 571.363 | 11263.333 | 496.867 |
| 32768 | 0 | per-topic | 3/3 | 371347/371347 | 121.892 | 128.109 | 10553.333 | 126.850 |
| 32768 | 0 | tcp-tls | 3/3 | 1203870/1203870 | 36.984 | 43.942 | 9033.333 | 195.168 |

callback trace에서 토픽 내 sequence 역전 관측: control-only/QoS0/32768B=21806, per-publish/QoS0/32768B=39041, per-topic/QoS0/32768B=34154, tcp-tls/QoS0/32768B=112532

수신 sequence 중복 관측: 0개.

지연은 trace의 unique 메시지로 실행별 계산 후 평균했다. 원본 callback.rs가 각 callback을 tokio::spawn으로 실행하므로 지연에는 callback 스케줄링이 포함되며, trace 역전을 네트워크 전달 순서 위반으로 단정할 수 없다. 전달 수는 publisher 성공 카운터와 수신 sequence를 대조했다. 명목 입력률은 publish 후 sleep 방식이므로 실제 생성률은 정책에 따라 달라질 수 있다. RAM은 50ms 간격 RSS 표본의 최대이며 Go TotalAlloc/ESP32 heap과 다른 지표다. broker CPU 관측에는 client 연결·warmup·종료 대기가 포함된다. CPU 0ms는 누적 CPU tick 증가를 관측하지 못했다는 뜻이다. 고부하에서 송신/수신 차이는 500ms drain 종료 시 미도착 수이며, 확정 패킷 손실과 다르다.

최적 방식 판정은 전달률·오류·메모리 예산을 먼저 만족해야 한다. 그 후 deadline 충족률/지연과 CPU 비용을 평가한다. 이번에는 네트워크 악화·실제 ESP32 자원 제한을 걸지 않았으므로 전체 우승자를 정하지 않는다.

원자료: `experiments/mqtt_quic_papers/results/author-saturation-verified`. 각 실행의 명령, 로그, 메시지 trace, QUIC 통계, 자원 표본과 manifest를 보존했다.
