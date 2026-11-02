"""Report fixed scheduled-load results with exclusions and application-pause scope."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directories',type=Path,nargs='+');p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();groups=defaultdict(list);manifests=[]
    for directory in a.directories:
        manifest=json.loads((directory/'manifest.json').read_text());manifests.append(manifest)
        for row in json.loads((directory/'summary.json').read_text()):
            groups[(row['scenario'],row['case'],row['qos'],row['policy'])].append(row)
    if len({m['workload_sha256'] for m in manifests})!=1:raise ValueError('different input schedules: do not merge')
    if any(m['args'].get('network','none')!='none' for m in manifests):
        raise ValueError('network impairment runs require a separate mobility report')
    settings=['budget','deadline_ms','drain_ms','pause_at_ms','pause_ms']
    if any(len({m['args'][key] for m in manifests})!=1 for key in settings):raise ValueError('different experimental settings')
    cfg=manifests[0]['args']
    lines=['# 고정 생성 입력 비교','',
        f'각 실행 {cfg["seconds"]}초 × {cfg["rate"]}개/초, payload {cfg["payload"]}B, 큐 payload 예산 {cfg["budget"]}B, deadline {cfg["deadline_ms"]}ms, drain {cfg["drain_ms"]}ms.',
        '실제 MQTT/H3 네트워크 송수신이다. 입력은 동일한 결정적 합성 바이트이며 카메라 JPEG·AI 부하가 아니다.',
        f'pause는 시작 {cfg["pause_at_ms"]}ms 뒤 송신 worker를 {cfg["pause_ms"]}ms 멈춘다. AP 전환·패킷 손실·QUIC migration을 재현하지 않는다.','',
        '| 조건 | 전송 | ACK 프로필 | 큐 정책 | 실행 완료/시도 | unique/생성 | deadline 충족률 | 수신 p95 평균 ms | 큐 최고 MiB | client RSS 최고 MiB |',
        '|---|---|---|---|---:|---:|---:|---:|---:|---:|']
    aggregation=[]
    for key,rows in sorted(groups.items()):
        scenario,case,qos,policy=key;ok=[r for r in rows if r['status']=='ok']
        total=sum(r.get('generated',0) for r in rows);got=sum(r.get('received_unique',0) for r in rows)
        deadline=sum(r.get('deadline_received',0) for r in rows)/total if total else None
        latency=[r['received_p95_ms'] for r in ok if r.get('received_p95_ms') is not None]
        queue=max((r.get('queue_peak_bytes',0) for r in rows),default=0)/1024**2
        rss=max((r.get('client_peak_rss_bytes',0) for r in rows),default=0)/1024**2
        ack='HTTP 200' if case=='h3-port' else f'MQTT QoS{qos}'
        lines.append(f'| {scenario} | {case} | {ack} | {policy} | {len(ok)}/{len(rows)} | {got}/{total} | '
            +(f'{deadline:.1%}' if deadline is not None else '미측정')+' | '+(f'{statistics.mean(latency):.2f}' if latency else '미측정')+f' | {queue:.2f} | {rss:.2f} |')
        aggregation.append(dict(scenario=scenario,case=case,qos=qos,policy=policy,attempted=len(rows),succeeded=len(ok),
            generated=total,received_unique=got,deadline_fraction=deadline,queue_peak_mib=queue,client_peak_rss_mib=rss,
            generation_lateness_max_ms=max((r.get('generation_lateness_max_ms',0) for r in rows),default=0)))
    lines += ['','## 해석 범위','',
        '- 모든 실행에 동일한 생성 스케줄을 주며 폐기된 메시지도 deadline 분모에 포함한다. 수신된 메시지만의 p95로 전체 성능을 평가하지 않는다.',
        '- 입력 시각 지연은 각 result에 따로 기록한다. source·바이너리·입력 SHA-256 및 raw 생성/제출/수신 기록을 보존한다.',
        '- `amr_latest_queue_only`는 ESP32의 실제 video_admission.c policy 1을 호출한다. RTT/cwnd/in-flight/RSSI를 모르는 기본 경로만 사용한 큐 admission 분리 실험이다. 전체 펌웨어 정책, AI 우선 처리, PSRAM guard, QUIC migration은 포함하지 않는다.',
        '- FIFO는 만료가 없고 deadline/AMR은 2초 만료를 적용한다. 그러므로 이는 transport와 큐 정책의 별도 축이며, FIFO 대비 차이를 QUIC만의 효과라고 부르지 않는다.',
        '- 큐 예산은 application payload + 진행 중 publish를 센다. MQTT/QUIC 라이브러리와 broker 내부 버퍼, allocator overhead는 별도다. 전체 ESP32 RAM 6MiB 제한을 달성한 결과가 아니다.',
        '- H3는 Saif의 HTTP method mapping을 Python/aioquic으로 구현한 별도 비교군이다. 원본 Go 코드·QUIC tuning 재현이 아니며 HTTP 200은 broker 처리 ACK로 MQTT QoS1과 같은 프로토콜이 아니다. 구현 언어 차이가 있으므로 CPU/RSS 차이를 프로토콜 고유 효과로 단정하지 않는다.',
        '- H3와 MQTT 모두 이번 표에서는 서버 인증 TLS + 익명 client를 사용한다. H3 Basic 인증/연결 캐시는 별도 기능 시험으로 확인한다.',
        '- 서버와 subscriber는 모두 loopback이다. 실험 장비는 다른 작업과 완전히 격리되지 않았고 반복은 2회이므로 미세 지연 차이의 통계적 우열을 주장하지 않는다.',
        '- 결론 범위는 고정 입력 및 application 송신 정지에 대한 기능·큐 동작이다. 실제 AP 핸드오프 수신 공백/복구 시간, 전력, ESP32 최적 방식은 미판정이다.','',
        '원자료: '+', '.join(str(p) for p in a.directories),'']
    with a.output.open('x') as out:out.write('\n'.join(lines))
    a.output.with_suffix('.json').write_text(json.dumps(aggregation,indent=2)+'\n')


if __name__=='__main__':main()
