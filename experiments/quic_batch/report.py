"""Render measured results and the explicit QoS trade-off."""
import json,sys
from pathlib import Path
p=Path(sys.argv[1]).resolve();base=Path(__file__).resolve().parent
meta=json.loads((p/'outcome.json').read_text());ss=json.loads((p/'aggregate.json').read_text());stability=json.loads((p/'stability.json').read_text());qos=json.loads((p/'qos-profile.json').read_text());runs=json.loads((p/'summary.json').read_text())
by={s['batch_kib']:s for s in ss}
def label(k):return f'{k} KiB' if k else 'frame'
def fmt(v,n=2):return '—' if v is None else f'{v:.{n}f}'
rel=p.relative_to(base)
lines=['# 1024×1024 JPEG QUIC batching / QoS 측정','',f'2026-09-21. [실행 자료]({rel}/outcome.json), [개별 조건]({rel}/summary.json), [반복 집계]({rel}/aggregate.json), [QoS 분석]({rel}/qos-profile.json), [그래프]({rel}/comparison.png).','',
'큰 묶음은 1 KiB 대비 수신 goodput을 약 32–42% 높이고 수신 KiB당 계측 QUIC I/O CPU를 약 19–25% 줄였지만, AI 평균 지연은 약 14–23% 늘렸다. 영상 DATAGRAM의 packetization은 121개/frame으로 유지됐고 영상 QUIC CPU/frame도 약 0.35–0.36 s로 큰 변화가 없었다. 따라서 전체 QoS 최적화를 위해 AI deadline과 영상 전송 예산을 함께 조정해야 한다.','',
'## 조건','',
'- ESP32-S3, ESPDet cat224, AI core1/priority1, TX core1/priority5. profile5의 AI metadata+preview 조건이며 ROI/evidence 전송은 꺼져 있다. 실제 1024×1024 JPEG 123,179 B 한 장을 반복한다. Replay 10 Hz, AI offer 상한 5 Hz, preview admission 시도 5 Hz. 모델 정확도/나사 탐지/VLM 평가는 포함하지 않는다.',
'- 동일 펌웨어에서 1/8/frame/4/16 → 16/4/frame/8/1 KiB 순서로 각 60 s 실행, 첫 10 s 제외, 조건 사이 8 s drain. 아래 수치는 첫·마지막 성능 snapshot으로 맞춘 구간의 집계다. 무선 채널은 통제하지 않았으며 반복 범위는 신뢰구간이 아니다.',
'- 모든 조건에서 같은 3 MiB 재사용 RGB 버퍼, 동일 JPEG 조각별 큐 할당, 2 s 영상 deadline, 20 ms lock-hold 목표, 기존 3 ms 외부 TX yield 검사, 소프트웨어 ChaCha20-Poly1305를 사용한다. 배치별 실행 기회 차이도 있으므로 효과를 mutex 자체의 명령 비용으로만 해석하지 않는다.',
'- 큐에서 꺼낸 배치도 in-flight 상태로 admission에 포함한다. 원래 1,000 B 조각은 이번 비교에서 모든 조건을 정확한 1,024 B로 통일했다.','',
'## 제출과 packetization','',
'각 프레임은 120×1,024 B + 299 B = **121개 application fragment**다. 12 B 헤더를 붙여 DATAGRAM에는 최대 1,036 B를 제출한다. 로컬 1,180 B 제한과 peer DATAGRAM-frame 제한을 검사한다. 4/8/16 KiB/frame은 묶음 API의 입력 크기이며, QUIC DATAGRAM 한 개의 크기가 아니다.','',
'| 묶음 | API 호출/frame 상당량 | 잠금/frame 상당량 | packetizer 호출/frame | UDP 출력/frame | 잠금 대기 ms/frame | 영상 QUIC CPU ms/frame |',
'|---|---:|---:|---:|---:|---:|---:|']
for k in [1,4,8,16,0]:
 s=by.get(k)
 if not s:lines.append(f'| {label(k)} | 정상 완료 없음 | — | — | — | — | — |');continue
 lines.append('| '+label(k)+' | '+' | '.join(fmt(s[x]) for x in ['submission_calls_per_sent_frame','locks_per_sent_frame','packetizer_calls_per_sent_frame','udp_outputs_per_sent_frame','batch_wait_ms_per_sent_frame','batch_cpu_ms_per_sent_frame'])+' |')
lines+=['','frame 상당량은 수락된 DATAGRAM 121개로 정규화했다. UDP 출력은 DATAGRAM packetizer가 실제 socket으로 보낸 출력으로, 다른 STREAM/control 경로의 패킷까지 포함한 전체 연결 패킷 수는 아니다. 프레임 단위 API도 pacing/lock-hold 예산 때문에 여러 번 잠금을 획득할 수 있다.','',
'## 수신 성능과 AI QoS','',
'| 묶음 | 정상 완료/시도 | AI mean / p95 s | AI 표본 / 실패 | 완성 JPEG KiB/s (반복 범위) | QUIC I/O CPU ms/수신 KiB | Admission drop | 수락 후 미완성 |',
'|---|---:|---:|---:|---:|---:|---:|---:|']
for k in [1,4,8,16,0]:
 s=by.get(k);t=next(x for x in stability if x['batch_kib']==k)
 if not s:lines.append(f'| {label(k)} | {t["completed"]}/{t["attempted"]} | — | — | — | — | — | — |');continue
 lo,hi=s['goodput_repeat_range']
 lines.append(f'| {label(k)} | {t["completed"]}/{t["attempted"]} | {s["ai_ms_mean"]/1000:.2f} / {s["ai_ms_p95"]/1000:.2f} | {s["ai_n"]} / {s["ai_failure_n"]} | {s["goodput_KiBs"]:.1f} ({lo:.1f}–{hi:.1f}) | {s["quic_cpu_ms_per_received_KiB"]:.2f} | {s["admission_drops"]}/{s["offered_frames"]} | {s["admitted_not_completed"]}/{s["admitted_frames"]} |')
lines+=['','goodput은 수신측에서 조각 순서/중복을 처리하고 전체 JPEG의 SHA-256이 고정 입력과 일치한 경우만 계산했다. Admission drop은 송신 전에 최신 프레임 정책으로 거절한 영상이며 network loss가 아니다. 미완성은 수락된 프레임을 cohort로 삼아 조건 종료 후 최대 8 s의 수신까지 확인한다.','',
'| 묶음 | 영상 TX CPU % | 전체 계측 QUIC I/O CPU % | batch 잠금 1회 평균 대기 ms | 전체 연결 잠금/s | 관측 queue peak KiB | 최소 internal heap KiB |',
'|---|---:|---:|---:|---:|---:|---:|']
for s in ss:lines.append(f'| {label(s["batch_kib"])} | {s["tx_cpu_pct"]:.1f} | {s["quic_io_cpu_pct"]:.1f} | {s["mean_batch_lock_wait_us"]/1000:.2f} | {s["all_locks_per_s"]:.1f} | {s["queue_peak"]/1024:.1f} | {s["min_internal_free"]/1024:.1f} |')
lines+=['','CPU %는 한 코어 100% 기준이다. QUIC I/O CPU는 batch API + process/read/STREAM/single-DATAGRAM API의 태스크 실행시간 합이며 Wi-Fi/ISR 전체 비용이 아니다. 현재 실행 중인 slice까지 포함하는 FreeRTOS 계측으로 lock 대기·preemption과 분리했다.','',
'## 현재 로그로 확인한 서비스 QoS','',
'| 실행 | 상태 | capture→AI ready 추정 p95 s | 완료 AI 결과→서버 수신 | 관측 최대 영상 공백 s |',
'|---|---|---:|---:|---:|']
for s in qos:lines.append(f'| {s["name"]} | {s["status"]} | {fmt(s["ai_ready_age_estimate_ms"]["p95"]/1000 if s["ai_ready_age_estimate_ms"]["p95"] is not None else None)} | {s["received_of_completed"]}/{s["completed_ai_results"]} | {s["preview_service"]["max_observed_silence_ms"]/1000:.2f} |')
lines+=['','capture→AI ready는 같은 MCU 시계로 계산하며 AI start + inference duration으로 완료 시점을 추정한다. 영상 공백은 같은 서버 시계에서 계산하고 관측 구간 양 끝의 잘린 공백을 포함한다. 서로 동기화하지 않은 MCU/server 시계를 빼서 one-way latency를 만들지 않았다.','',
'AI 결과 전달율은 GT 이벤트 DESR/F1이 아니다. 현재 trace에서 검출 성공이나 중요한 실제 이벤트의 deadline 전달을 증명하지 않는다. [QoS 로드맵](../aiot_qos/ROADMAP.md)의 event ID·시계 불확실성·GT 계측을 통해 다음 단계에서 검증한다.','',
'## 안정성 / 제외한 실행','',
'- 최초 smoke (`batch-20260920T173504Z`)에서는 반복 RGB 할당 실패로 큰 배치의 AI workload가 사라졌다. 정식 비교에서 제외했고, 모든 조건에 동일 재사용 버퍼를 적용했다.',
'- 동일한 보정 펌웨어의 첫 formal (`batch-20260920T174107Z`)에서 frame 조건은 약 13.7 s에 CPU1 idle watchdog 경고가 발생했다. 미리보기를 중단하고 전체 플래시를 복원했다. 이후 두 반복이 완료되더라도 이 재현 이력을 지우거나 장기 안정성이 입증됐다고 주장하지 않는다.',
'- 첫 flash에서 직렬 포트의 동시 접근으로 검증 오류가 발생했다. 다시 flash 후 성공을 확인했으며 측정 자료로 사용하지 않았다. 현재 runner는 flash→측정→복구 순서로 직렬 포트를 독점한다.']
for t in meta['stages']:
 if t.get('status')!='completed':lines.append(f'- 이번 sweep: {t["name"]}, {t.get("status")}, {t.get("end",t["start"])-t["start"]:.1f} s 후 중단: {t.get("reason",meta.get("reason",""))}.')
lines+=['','## 해석과 다음 단계','',
'- 영상 QUIC CPU/frame과 packetizer 출력 수를 함께 확인하면 묶음 API가 암호화·패킷 생성 작업 자체를 얼마나 줄였는지 구분할 수 있다. 본 구현은 application fragment 및 DATAGRAM별 packetization을 그대로 유지한다.',
'- 호출/잠금 수 감소와 성공 바이트당 CPU 절감 여부를 확인하되, AI tail latency·영상 공백·안정성까지 함께 평가한다. 높은 goodput 하나로 최적 설정을 선정하지 않는다.',
'- 현재 데이터는 짧은 두 반복의 characterization이다. 고정 JPEG, 모델/코어/priority, 비통제 Wi-Fi라는 조건에 한정된다. p99는 표본이 작아 안정적인 tail 추정이 아니다.',
'- 다음은 QoS 계측 완성 → 가장 좋은 고정 보호 설정 → workload 범위에서 고정 설정이 충족하지 못할 때 피드백 제어 순서다. VLM/split은 같은 end-to-end QoS 목표의 후속 축으로 유지한다.','',
'## 검증 / 복구','',
'- 호스트 C 테스트: 다섯 크기, pacing, deadline, payload 상한, UDP 실패, lock ownership, CPU accounting 통과. 수신 분석 테스트: 재정렬·중복·누락·잘못된 JPEG 해시 처리 통과. ESP-IDF 실험 빌드 성공.',
'- 펌웨어/소스 해시는 [artifact manifest](results/formal-artifacts.json)에 보존했다. 실험 옵션은 기본 OFF이며 상시 펌웨어에 자동 적용하지 않는다.',
'- 원래 전체 플래시 복구와 esptool hash 검증을 완료했다. profile0/probe0/mode2/policy1/video30·6144 KiB ACK 및 AI core0 실행 7회를 확인했다. [복원 로그](results/restore-flash.log), [복원 검증](results/restore-verification.json). NVS에는 저장하지 않았다.','']
(base/'RESULTS.md').write_text('\n'.join(lines));print(base/'RESULTS.md')
