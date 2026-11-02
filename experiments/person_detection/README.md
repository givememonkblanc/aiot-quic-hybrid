# ESP32 사람 FOMO · 영상 · 감지 스냅샷 시험

현재 목적은 ESP32 내부 사람 중심점 검출과 같은 프레임의 감지 사진 전송이다. 서버 YOLO는 학습용 자동 라벨에만 사용하며 운영 추론은 ESP32 TFLM이다. FOMO 중심점/활성 셀 영역은 사람 전체 bounding box나 얼굴 검출 영역이 아니다. 얼굴 식별 기능은 없다.

## 데이터와 학습 상태

- 로컬 수집: `datasets/person_live/session_01/manifest.jsonl`, 카메라 원본 JPEG만 로컬 저장. 수집은 300초로 제한했다.
- 공개 보완 데이터: [Ultralytics COCO128](https://github.com/ultralytics/assets/releases/download/v0.0.0/coco128.zip)의 128장, COCO class 0 사람 라벨 사용.
- 기울어진 현장 장면은 로컬 YOLO11m에 회전시켜 입력하고 라벨 좌표를 원래 방향으로 역변환했다. 확신이 낮은 현장 이미지를 빈 장면으로 단정하여 학습시키지 않았다.
- 학습에 사용한 현장 positive는 83장. 공개 데이터 이미지 단위 분리, 현장 데이터 시간 구간 분리 후 학습 164장 / 검증 47장. 좌우 반전 증강은 학습 분리 후 적용했다.
- 모델: ImageNet 사전학습 MobileNetV2 alpha 0.35의 stride-8 특징 + 2-class FOMO head. 입력 96×96 luma [0,1], 그래프 내부 [-1,1] 정규화, 출력 12×12×2. full-int8 모델 57,776B.
- **초기 시험 모델이며 일반 사람 탐지 성능은 부족하다.** 선택 임계값 0.3에서 현장 검증은 중심점 매칭 precision 0.70 / recall 1.00(21 TP, 9 FP)이지만, 현장 검증에는 신뢰할 수 있는 빈 장면 세트가 없다. 공개 영상 성능은 낮다. 자세한 값은 `out/cluster-validation.json`을 확인한다.
- 중심점 매칭 허용 반경은 출력 격자 1.5칸이며, 이 수치는 mAP/사람 식별 정확도가 아니다. 검증 데이터로 임계값을 정했으므로 독립 테스트 성능도 아니다. 현장 원본은 이전 전송기의 부분 JPEG가 섞였을 가능성이 있어 추가 정제/수집이 필요하다.
- 기존 주차 모델 C 파일/metadata를 덮어쓰지 않는다. 모델 경로를 별도 빌드 옵션으로 선택한다.

```bash
python3 experiments/person_detection/collect.py --seconds 300 --out datasets/person_live/session_02
# prepare.py는 현재 session_01과 공개 데이터를 읽는다.
python3 experiments/person_detection/prepare.py
python3 experiments/person_detection/train.py
python3 experiments/person_detection/evaluate.py
source /home/ryzen395/esp-idf/export.sh
bash experiments/person_detection/build.sh UNIQUE_VERSION
```

## 영상과 비동기 추론

OV2640의 UXGA 센서 윈도우를 1200×1200 영역에서 1024×1024로 출력한다(v12; 이전 v8~v11은 640×640). JPEG quality는 12에서 10으로 변경했다(낮을수록 압축이 적음). 수신 JPEG dimensions로 확인하며 서버 확대가 아니다. PSRAM 프레임 버퍼 2개, 최신 프레임 캡처, 유한한 대기/추론 JPEG 버퍼를 사용한다. 대기 중 오래된 AI 입력은 최신 프레임으로 교체한다. AI 결과는 입력으로 사용한 JPEG 생성 시각과 함께 보내고 서버는 동일 connection ID + frame timestamp 사진에만 연결한다.

24FPS는 요청 목표/스케줄 상한이며 달성값이 아니다. OV2640 [제조사 자료](https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-s2-kaluga-1/datasheet/Camera_OV2640.pdf)는 UXGA/SXGA 15FPS, SVGA 30FPS를 명시한다. 현재 square 경로는 UXGA 모드여서 24FPS 충족을 주장할 수 없다. 이전 640×640 프로필에서 대략 9~10FPS가 관찰되었으며, 1024×1024 프로필의 달성값으로 해석하지 않는다; 최신 측정은 `results/rates.json`(10초 창)을 본다. 24FPS가 우선이면 640×480/SVGA 센서 모드의 별도 검증 또는 하드웨어 변경이 필요하다.

고해상도 원본은 256KiB로 제한하며 이를 넘는 JPEG는 잘라 보내지 않고 건너뛴다. JPEG 길이 경로를 32비트로 확장했다. 카메라 큐 descriptor 6,400개는 PSRAM에 두고 실제 payload 예산은 별도로 제한한다(기본 256KiB, 최대 6MiB; 작은 감지/진단에 8KiB 추가 여유). 새 프레임 전체의 예상 할당량과 PSRAM 128KiB 여유를 확보한 경우에만 넣는다. AI는 1024×1024 JPEG를 1/8 디코드한 128×128 영상에서 기존 96×96 모델 입력을 생성하므로 전체 해상도 RGB 버퍼를 추가하지 않는다.

수신기는 같은 connection/frame의 연속 sequence JPEG 조각만 조립한다. 누락/순서 변경은 해당 프레임을 버린다. 손상된 조각을 이어 붙여 FPS를 부풀리지 않는다. 현 방식은 순서가 뒤바뀐 프레임을 복원하지 못한다.

## 서버와 원격 확인

`python3 experiments/person_detection/snapshots.py`는 기존 receiver feed를 읽어 마지막 감지 사진만 유지한다. ESP32 결과와 같은 프레임만 사용하며 서버에서 YOLO 추론은 하지 않는다.

- 기존 인증된 대시보드: 연속 MJPEG 영상 + 사람 감지 사진 전용 `/person-stream`. 기존 2초 스냅샷 저장 제한을 제거하고 정확히 일치한 프레임/감지 결과마다 저장한다. 브라우저 사진은 1.5초 API 폴링을 기다리지 않고 별도 MJPEG 연결로 수신한다. 새 감지가 없으면 마지막 사진과 경과 시간을 표시한다. 실제 갱신 속도는 보드 추론과 완성 프레임 수신에 제한된다.
- `/person-marked.jpg`, `/person-original.jpg`: 기존 로그인 보호 아래의 최신 감지 사진.
- `results/snapshot.json`: 감지 시각/신뢰도/프레임 정보. 오래된 사진을 현재 감지로 해석하지 않는다.
- `results/rates.json`: 최근 10초 완전한 프레임 수신 FPS와 AI 결과 수신 FPS.
- RAM: `experiments/memory/results/ram.csv` 연속 기록 유지.

프로세스는 현재 서버 세션에서 분리 실행 중이며 서버 재부팅 자동 시작은 아직 구성하지 않았다. OTA TLS identity와 기존 인증을 유지한다. 무인 경보/외부 사진 전송은 추가하지 않았다.

## 메모리 압박 비교 (v19)

USB 콘솔의 `video <목표 FPS> <큐 예산 KiB>`로 1~30 FPS와 64~6144KiB를 설정한다. 런타임 설정이며 NVS에 저장하지 않는다. 목표 FPS는 실제 센서 캡처/수신 FPS와 다르며, 복제 이미지를 새 프레임으로 세지 않는다.

- `mode 1`: QUIC migration + FIFO 영상 큐. 영상에 age deadline을 부여하지 않아 누적 지연과 메모리 점유를 비교한다.
- `mode 2`: 영상 backlog 억제, QUIC cwnd/in-flight/RTT 기반 admission, 감지 결과 큐 우선, 영상 2초 deadline(v18; 118KB 장면에서 v17의 1초는 지나치게 짧았음). 한 조각 전송에 실패한 프레임은 남은 조각도 폐기한다.
- MQTT 제어/진단은 STREAM, 영상과 감지 결과는 DATAGRAM을 유지한다. MP-QUIC 다중 경로 구현을 주장하지 않는다.
- AI는 송신 큐에 완전히 들어간 원본에만 실행하여 전송하지 않은 프레임의 추론 낭비를 줄인다. 패킷 손실로 완성되지 않은 사진은 서버에서 감지 결과와 합치지 않는다.
- `experiments/memory/results/queue.csv`에는 payload 점유/최고점/예산, 목표 FPS, 큐 깊이와 실패/만료 수를 별도로 기록한다. payload 바이트는 allocator/descriptor 오버헤드를 제외하며 heap 사용률과 다르다.

```bash
python3 experiments/person_detection/pressure_trial.py --fps 30 --budget-kib 6144 --seconds 35 --output experiments/person_detection/results/UNIQUE_RUN
```

시험은 FIFO/적응형 각 35초이며 재부팅, RAM 측정 25초 중단, 내부 free heap 16KiB 미만이면 중단한다. 마지막에는 `mode 2`, `video 15 256`을 복원한다. 실제 핸드오프 비교는 같은 입력/큐 예산에서 별도 실시해야 한다. 순차 live 장면의 내용 차이와 10초 FPS 창/5초 RAM 주기를 고려해야 하며 이 짧은 비교만으로 일반 성능을 주장하지 않는다.

v16 첫 압박 시험은 PSRAM 약 97%에서 heap tail 손상으로 중단됐다. ngtcp2 block allocator가 base alignment 12(mod 16)에서 할당 경계를 4바이트 넘을 수 있음을 회귀 테스트로 재현했다. v17은 정렬 여유를 8바이트에서 최대 15바이트로 수정했다. heap light canary를 켠 동일 빌드에서 두 정책을 비교하며, 진단 비용을 포함한 수치다. 과거 v10~v16의 재부팅을 단순 OOM 또는 스케줄러 성능으로 해석하지 않는다.

v18부터 QUIC 세션 종료 시 crypto context/수신 버퍼 해제를 다시 활성화했다. v19는 수신 패킷마다 수행하던 전체 heap 순회를 `AIOT_QUIC_HEAP_DIAGNOSTICS` 진단 옵션으로 분리한다(기본 꺼짐). 할당 경계의 light canary는 유지한다. 이 변경 전후 수치를 동일 펌웨어 결과로 합치지 않는다.

AMR 정책 원본 8종의 실제 구현·로그·ESP32 적용 범위 비교는 [AMR 비교 보고서](../amr_comparison/README.md)를 참고한다.
