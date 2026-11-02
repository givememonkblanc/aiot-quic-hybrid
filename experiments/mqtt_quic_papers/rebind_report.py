"""Separate actual loopback NAT-rebinding results from application-pause tests."""
import argparse
import json
from pathlib import Path
import statistics


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directories',type=Path,nargs='+');p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();data=[];hashes=set();settings=set()
    for directory in a.directories:
        m=json.loads((directory/'manifest.json').read_text());hashes.add(m['workload_sha256'])
        cfg=m['args'];settings.add(tuple(cfg[k] for k in ['budget','pause_at_ms','pause_ms','drain_ms','payload']))
        if cfg.get('network') not in ['udp-pass','udp-rebind']:raise ValueError('not a network fixture run')
        rows=json.loads((directory/'summary.json').read_text())
        mode='relay control' if cfg['network']=='udp-pass' else 'active migrate()' if cfg.get('active_migration') else 'passive rebind'
        for policy in sorted({r['policy'] for r in rows}):
            group=[r for r in rows if r['policy']==policy]
            values=[r['first_fresh_receive_after_network_resume_ms'] for r in group if r.get('recovery_verified')]
            data.append(dict(mode=mode,policy=policy,runs=len(group),execution_completed=sum(r['status']=='ok' for r in group),
                generated=sum(r.get('generated',0) for r in group),received=sum(r.get('received_unique',0) for r in group),
                on_time=sum(r.get('deadline_received',0) for r in group),
                queue_peak_bytes=max(r.get('queue_peak_bytes',0) for r in group),
                verified_recoveries=sum(bool(r.get('recovery_verified')) for r in group) if cfg['network']=='udp-rebind' else None,
                recovery_mean_ms=statistics.mean(values) if values else None,
                recovery_samples_ms=values,raw_directory=str(directory)))
    if len(hashes)!=1 or len(settings)!=1:raise ValueError('unequal workload/settings')
    data.sort(key=lambda r: ({'relay control':0,'passive rebind':1,'active migrate()':2}[r['mode']], {'fifo':0,'deadline':1,'latest':2,'amr_latest_queue_only':3}[r['policy']]))
    lines=['# Loopback NAT 재바인딩 시험','',
        '실제 MQTT QoS1 / QUIC 단일 STREAM을 사용한다. 30개/초 × 6초, 256KiB payload, 6MiB application queue, 2초 deadline 및 종료 drain이다.',
        '동일 중계를 거치는 정상 대조군, 3초 양방향 UDP blackhole 후 upstream source port 변경, 그리고 복구 20ms 뒤 양쪽 client의 저자 migrate() API 호출을 비교한다. 이는 AP 핸드오프/ESP32 시험이 아니다.','',
        '| 방식 | 큐 정책 | 실행 완료/시도 | 수신/생성 | deadline 충족 | 큐 최고 MiB | 검증된 복구/시도 | 새 입력 수신까지 평균 ms |',
        '|---|---|---:|---:|---:|---:|---:|---:|']
    for r in data:
        verified='해당 없음' if r['verified_recoveries'] is None else f'{r["verified_recoveries"]}/{r["runs"]}'
        latency='미복구/해당 없음' if r['recovery_mean_ms'] is None else f'{r["recovery_mean_ms"]:.2f}'
        lines.append(f'| {r["mode"]} | {r["policy"]} | {r["execution_completed"]}/{r["runs"]} | {r["received"]}/{r["generated"]} | '
            f'{r["on_time"]/r["generated"]:.1%} | {r["queue_peak_bytes"]/1024**2:.2f} | {verified} | {latency} |')
    lines += ['','## 복구 판정','',
        '단순히 QUIC 객체가 남아 있거나 publish가 성공했다고 복구로 세지 않는다. 다음 조건을 모두 확인한다.',
        '', '1. publisher와 subscriber 각각 시작/종료 Quinn stable_id 동일, 종료 원인 없음.',
        '2. 두 client 모두 PATH_CHALLENGE 수신 및 PATH_RESPONSE 송신 카운터 증가.',
        '3. network 복구 시각 이후 생성한 실제 payload가 subscriber callback에 도달.',
        '', 'raw.json의 transport_evidence/migration_events, relay.json의 단절·원래/새 source port·forward/drop 카운터를 보존한다.',
        '', '지연 기준은 fixture의 명목 network 복구 시각이며 실제 중계 rebind 시각은 relay 로그에 별도로 있다. 루프 타이머 오차를 포함한다. 복구된 실행만의 지연 평균 옆에 복구 성공 횟수를 함께 표시한다.',
        '', 'passive rebind에서 수동적으로 수신만 하는 subscriber는 새 NAT tuple을 서버에 알릴 트래픽이 없을 수 있다. 8초 관측 안에 미복구인 것은 영구 복구 불능이라는 뜻이 아니다. 기본 MQTT keepalive/재전송 timeout 이후는 이번 관측 범위 밖이다.',
        '', 'active migrate()는 publisher/subscriber endpoint를 새 로컬 UDP socket에 rebind한다. 논문의 저자 라이브러리 API를 호출한 호스트 실험이다. ESP32의 ngtcp2 구현이나 Wi-Fi link-up 감지에 이 효과가 그대로 적용됐다고 주장하지 않는다.',
        '', '중계는 kernel UDP buffer와 Python CPU 비용을 추가한다. 그래서 udp-pass 대조군을 포함한다. 패킷 손실 확률·RTT·전력·실제 RF 이동은 통제하지 않았다. 작은 반복 수이므로 수십 ms 차이의 통계적 우열은 주장하지 않는다.','']
    with a.output.open('x') as out:out.write('\n'.join(lines))
    a.output.with_suffix('.json').write_text(json.dumps(data,indent=2)+'\n')


if __name__=='__main__':main()
