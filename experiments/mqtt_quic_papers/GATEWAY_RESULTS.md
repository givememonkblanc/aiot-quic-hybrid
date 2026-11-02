# QUIC→TLS gateway 기능 검증

논문의 구조를 현대 Python/aioquic으로 옮긴 기능 시험이다. 저자 코드, cloud, 0-RTT, ESP32 비교 결과가 아니다.

| 경로 | MQTT QoS | 실행 | unique 수신/생성 |
|---|---:|---:|---:|
| direct-tls | 0 | 3 | 90/90 |
| direct-tls | 1 | 3 | 90/90 |
| quic-gateway-tls | 0 | 3 | 90/90 |
| quic-gateway-tls | 1 | 3 | 90/90 |

gateway→broker TLS 연결 12개가 모두 TLSv1.3으로 확인됐다. 두 구간 모두 서버 인증서를 검증했다. gateway 오류 0개.

MQTT CONNECT/SUBACK, QoS1 PUBACK 및 구독 전달을 실제 교환한다. direct TLS와 gateway 경로는 같은 loopback Mosquitto TLS broker를 사용한다. 전력/CPU/RAM 우열은 판정하지 않는다. 이 기능 시험은 Rust adapter 빌드와 일부 겹쳤으므로 지연 순위의 근거로 사용하지 않는다.

원자료: `experiments/mqtt_quic_papers/results/gateway-functional-01`. 실행 코드 사본과 source hash는 manifest에 있다.
