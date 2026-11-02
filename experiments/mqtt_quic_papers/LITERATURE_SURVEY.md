# MQTT-over-QUIC 문헌 확장 조사

확인일: 2026-09-20. 학술 원문, 저자 저장소, 학회·연구기관 자료를 우선했다. 아래는 확인 범위 내 목록이며 모든 출판물을 망라했다는 뜻은 아니다. 원문 검토, 초록 검토, 서지 확인을 구분한다. 논문 공개는 재현 성공을 뜻하지 않는다.

## 직접 구현·평가 연구

| 연구 | 구조·비교 대상 | 실험과 지표 | 확인 수준 / 재현 자료 |
|---|---|---|---|
| Kumar & Dezfouli, 2019, **Implementation and Analysis of QUIC for MQTT** | Paho/Mosquitto와 Google QUIC 사이 agent 통합. MQTT/TCP와 비교 | Pi 3, 유선·간섭 Wi-Fi·장거리. 연결 비용, 지연, half-open 연결 자원, 주소 변경 시 처리량 | 원문 검토. 현대 IETF QUIC와 버전·암호화 조건이 다름. [저자 원문](https://www.cse.scu.edu/~bdezfouli/publication/QUIC-MQTT-COMNET2019.pdf) |
| Fernández et al., WiMob 2020, **And QUIC Meets IoT: Performance Assessment of MQTT over QUIC** | Go 기반 MQTT/QUIC 평가 계열의 초기 논문 | 후속 2021 논문이 실험 확장을 명시 | 서지 및 후속 논문 확인; 이번 조사에서 원문 전체는 미확보. [DOI](https://doi.org/10.1109/WiMob50308.2020.9253384), [저자 소속기관 목록](https://personales.unican.es/aguerocr/Misc/RamonAguero_Short_CV_EN.pdf) |
| Fernández et al., Sensors 2021, **Even Lower Latency in IIoT: Evaluation of QUIC in Industrial IoT Scenarios** | Paho/VolantMQ에 quic-go 통합. MQTT/TCP+TLS 대 MQTT/QUIC | Linux 컨테이너+ns-3. 단일/다중 publisher, 공유 Wi-Fi, RTT·손실·대역폭·MAC 재전송. 메시지군 완료 시간과 연결을 포함한 전달 시간 | 원문 검토, 저자 코드 확보. [원문](https://pmc.ncbi.nlm.nih.gov/articles/PMC8434189/) |
| Jeddou et al., Sensors 2022, **Delay and Energy Consumption of MQTT over QUIC: An Empirical Characterization Using Commercial-Off-The-Shelf Devices** | 실제 Pi publisher→broker→subscriber. TCP+TLS와 QUIC, QoS0/1/2. 멀티스트림 평가는 제외 | Ethernet+tc로 무선 조건 모사. 단방향 지연 분포·RSD, 외부 계측 에너지 | 원문 검토, 실험 코드 확보. [원문](https://pmc.ncbi.nlm.nih.gov/articles/PMC9145551/), [코드](https://github.com/tlmat-unican/MQTT-QUIC-for-RaspberryPi) |
| Alqattaa, Loebenberger & Möges, COINS 2022, **Analyzing the Latency of QUIC over an IoT Gateway** | 장치↔gateway는 MQTT/QUIC, gateway↔cloud는 MQTT/TLS 1.3 | gateway 유무 및 full handshake/0-RTT 조건의 지연 비교 | 연구기관 초록 확인. 종단 전체가 QUIC인 구조로 해석하지 않음. 세부 grid·코드 미확인. [기관 자료](https://publica.fraunhofer.de/entities/publication/f6c226e4-a4dc-4468-b063-c85386e0efb7) |
| Luna, Farias & Cavalcanti, SoftCOM 2025, **MQTT Over QUIC: Comparison with TCP and TCP+TLS in a Virtualized IoT Environment** | 제목에서 QUIC/TCP/TCP+TLS 가상화 비교 확인 | 본문을 확보하지 못해 세부 설정·개선율·코드는 미확정 | 학회 공식 프로그램과 DOI 서지 확인. [학회 프로그램](https://2025.softcom.fesb.unist.hr/wp-content/uploads/2025/09/FINAL_PROGRAM_2025.pdf), [DOI](https://doi.org/10.23919/SoftCOM66362.2025.11197418) |
| Tsurumi & Asaka, IEICE CQ2026-45, 2026-07, **Performance Evaluation of MQTT over QUIC in LEO Satellite Communication Environments** | MQTT/QUIC 대 TCP, LEO 위성 환경 | 초록은 네트워크 emulation 및 실제 Starlink에서 메시지 전송 시간 평가를 명시 | 학회 초록 확인. 일본어 포스터 기술보고서 4쪽; 전체 설정·코드 미확인. [학회 자료](https://www.ieice.org/publications/search/summary.php?id=144459&lang=en&tbl=ken) |

Kumar의 마이그레이션 실험은 10 publisher/100 connection, 5분 간격 broker 인터페이스 IP 변경, 총 16분이다. 실제 ESP32 AP 로밍 시험으로 바꿔 설명하면 안 된다. CPU/메모리 개선 수치는 half-open 연결 실험의 맥락이며 모든 정상 상태의 자원 절감을 뜻하지 않는다.

Fernández의 2020→2021, Jeddou의 2022는 연관 구현 계열이다. 독립적인 세 QUIC 스택이 같은 결과를 보인 것처럼 세지 않는다. 기존 원본 clone의 절대경로 replace 의존성 문제도 여전히 남아 있다.

## 직접 관련된 2026 공개 원고와 분석 모델

### Fabricio Bracht — Evaluating Stream Mapping Strategies for MQTT over QUIC

[원고 소스](https://github.com/LabOverWire/mqtt-lib/blob/main/publications/comnet/submission/main.tex), [재현 안내](https://github.com/LabOverWire/mqtt-lib/tree/main/publications/comnet), [구현](https://github.com/LabOverWire/mqtt-lib).

- publication 폴더 README는 **2026년 3월 Computer Networks 투고**라고 명시한다. 최상위 README의 저널 표기만으로 게재 확정이라고 쓰지 않는다.
- 단일 스트림, 토픽별 스트림, PUBLISH별 스트림을 비교한다. QoS0 DATAGRAM 확장도 평가한다.
- GCP VM/Quinn 0.11/tc-netem, 256B, 설정당 15회. 연결, HOL, 손실별 처리량, 토픽 수, DATAGRAM 대 STREAM 실험과 frame packing ablation을 제공한다. 원고는 HOL 실험에서 pub/sub를 같은 호스트에 배치했다고 명시한다.
- 지연 p50/p95/p99뿐 아니라 토픽 간 지연 퍼짐과 지연 급증의 동시 발생 정도를 분석한다. 이것을 단순 지연 개선율과 혼동하지 않는다.
- 원고는 처리량 비교의 TCP가 비암호화이고, QUIC은 암호화임을 인정한다. README는 TCP CUBIC/QUIC NewReno 차이도 기록한다. 개선율을 프로토콜 하나의 효과로 단정할 수 없다.
- ESP32 및 마이그레이션 평가는 향후 과제다. 코드의 embedded parser 지원은 ESP32에서 전체 QUIC 스택 실험을 했다는 증거가 아니다.
- 원고의 EMQX 단일 스트림 주장과 비교표는 [EMQX의 2025 공식 멀티스트림 설명](https://www.emqx.com/en/blog/multi-stream-of-mqtt-over-quic)과 충돌한다. 선행연구·최초성 주장은 별도 검증이 필요하다.

원고와 README 사본은 `references/stream_mapping2026*`에 저장했고 SHA-256은 `survey_sources_20260920.json`에 기록했다. README가 연결한 [Zenodo 데이터](https://doi.org/10.5281/zenodo.19098820)는 이번에 다운로드·검증하지 않았다. 클라우드 배포나 실험 스크립트 실행은 하지 않았다.

### Piotrovskyi — Parametric Evaluation of MQTT 5 over QUIC under Mobility Scenarios

[출판사 초록](https://naukajournal.org/index.php/ISMSD/article/view/2776), 2026, DOI 10.26886/2414-634X.3(72)2026.2.

Wi-Fi/4G/5G, 두 handover, NAT rebinding의 모델을 비교한다. **초록이 QUIC 예측은 실측 검증하지 않았다고 명시한다.** Docker+EMQX+tc/netem 검증은 TCP+TLS 쪽이다. QUIC 이동성 실험 성공 사례나 ESP32 재현 기준으로 사용하지 않는다. L2 단절과 전송계층 복구를 분리하는 실험 설계 관점은 참고할 수 있다.

## MQTT-over-QUIC을 비교군으로 사용하는 H3 연구

| 연구 | 실제 연구 대상 | MQTT와의 관계 |
|---|---|---|
| Saif & Matrawy, **A Pure HTTP/3 Alternative to MQTT-over-QUIC in Resource-Constrained IoT**, CSCN 2021 | H3 기반 pub/sub | MQTT/QUIC을 비교군으로 사용. 실행·첫 데이터 시간·CPU·heap·wire 비용. [저자 원문](https://arxiv.org/abs/2106.12684) |
| Saif & Matrawy, **An Experimental Investigation of Tuning QUIC-Based Publish–Subscribe Architectures in IoT**, IEEE IoT Journal 11(3), 2024, 4924–4933 | H3와 MQTT/QUIC의 IoT tuning | Pi Zero+VM+NetEm, 50회, QoS0/1, 8개 KPI. [검토한 2023 저자 수정본](https://arxiv.org/abs/2208.11178), [정식 DOI](https://doi.org/10.1109/JIOT.2023.3302160) |
| Saif & Matrawy, **Mass Configuration of Heterogeneous IIoT Nodes: A Proposal and Experimental Evaluation**, ICNC 2024 | H3 기반 대규모 장치 관리 | MQTT/QUIC 및 SNMP와 비교. Docker 최대 500 client, 20회. 장기 연결을 가정해 연결 수립 비용은 지표에서 제외. [학회 제공 원문](https://www.conf-icnc.org/2024/papers/p932-saif.pdf) |

앞서 tuning 논문을 2022 공개/2023 수정본으로 설명한 것은 읽은 원고의 날짜였다. 정식 서지는 **2024 IEEE IoT Journal**, DOI에 2023이 들어간다. DOI 등록 메타데이터도 확인했다. ICNC 문서는 검색에서 제공된 학회 원문 본문을 확인했지만 로컬 PDF 다운로드는 인증서 검증 실패로 완료하지 못했다.

## 구현 참고자료와 논문을 구분

- [EMQX MQTT over QUIC](https://www.emqx.com/en/blog/mqtt-over-quic): 실제 제품/벤더 평가 자료. 학술 논문으로 세지 않는다.
- [EMQX 멀티스트림 실험](https://www.emqx.com/en/blog/multi-stream-of-mqtt-over-quic): 제어·작은 메시지·큰 메시지의 스트림 분리 및 우선순위 예시. 우리 제어/감지/영상 혼합 부하와 관련된 구현 참고자료다.
- [Paho fork](https://github.com/pgOrtiz90/paho.mqtt.golang), [VolantMQ fork](https://github.com/fatimafp95/volantmq_2): Fernández/Jeddou 계열의 직접적인 구현 출발점. 최신 버전으로 교체하면 원본 재현과 구분해야 한다.
- STORM, ETC 등의 일반 MPQUIC 스케줄러는 MQTT-over-QUIC 직접 연구 목록에 합치지 않는다.
- Scribd에서 발견된 AIoT/LLM 문서 및 출판사·저자 원문을 확인하지 못한 자료는 확정 논문 목록에서 제외했다.

## 우리 연구에 대한 판단

이번 확인 범위에서는 ESP32의 카메라·추론 동시 부하, 제한 RAM, 실제 Wi-Fi 핸드오프, MQTT의 전달 의미를 함께 평가한 직접 증거를 확보하지 못했다. 이것은 검색 결과의 한계이며 최초성의 증명은 아니다.

비교 우선순위는 (1) Fernández/Jeddou의 실제 MQTT pub/sub 기준선, (2) Saif의 자원 측정 정의, (3) Bracht/EMQX의 메시지 분리 방식, (4) Kumar의 주소 변경 실험을 실제 AP 핸드오프로 확장하는 순서가 적절하다. 이는 우리 설계 판단이다.

MQTT-over-QUIC과 MPQUIC는 별개 축이다. 멀티스트림은 한 연결에서 메시지 흐름을 나누는 방식이며, 멀티패스는 여러 경로를 동시에 쓰는 방식이다. MQTT 마이그레이션 지원만으로 MPQUIC 실험이 되지는 않는다.

DATAGRAM 사용 여부만으로 MQTT 관련성을 판정하지 않는다. Bracht는 MQTT 메시지 처리 체계에 DATAGRAM 전달을 추가한 확장이다. 현재 ESP32의 자체 JPEG 조각 DATAGRAM은 MQTT PUBLISH·broker 토픽 전달 경로가 아니므로 MQTT-over-QUIC으로 부를 수 없다. 표준 reliable STREAM 기준선과 손실 허용 확장은 비교표에서 분리해야 한다.
