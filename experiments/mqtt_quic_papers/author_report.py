"""Aggregate actual author-run measurements, preserving workload boundaries."""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import re
import statistics


def mean(rows,key,scale=1):
    values=[r[key]*scale for r in rows if r.get(key) is not None]
    return statistics.mean(values) if values else None


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    groups=defaultdict(list)
    data=json.loads((args.directory/'summary.json').read_text())
    for row in data:
        name=f'{row["case"]}-q{row["qos"]}-b{row["payload_bytes"]}-r{row["run"]}'
        path=args.directory/name
        match=re.search(r'published: (\d+), received: (\d+)',(path/'client.log').read_text())
        if match:
            row['published']=int(match[1])
        values=[];sequences=set();duplicates=0;reordered=0;last_sequence={}
        for file in (path/'traces').glob('*.csv'):
            with file.open() as stream:
                reader=csv.DictReader(stream)
                if not {'topic_idx','seq','latency_us'}.issubset(reader.fieldnames or []):
                    continue
                for sample in reader:
                    key=(sample['topic_idx'],sample['seq'])
                    if key in sequences:
                        duplicates+=1
                    else:
                        sequences.add(key);values.append(float(sample['latency_us']))
                        topic,seq=sample['topic_idx'],int(sample['seq'])
                        if seq<last_sequence.get(topic,-1):
                            reordered+=1
                        last_sequence[topic]=max(seq,last_sequence.get(topic,-1))
        row['received_unique']=len(sequences)
        row['duplicates']=duplicates
        row['per_topic_order_violations']=reordered
        row['delivery_ratio']=len(sequences)/row['published'] if row.get('published') else None
        row['latency_mean_us']=statistics.mean(values) if values else None
        if values:
            from paper_metrics import quantile
            row['latency_p95_us']=quantile(values,95)
            row['latency_p99_us']=quantile(values,99)
        groups[(row['payload_bytes'],row['qos'],row['case'])].append(row)
    aggregates=[]
    lines=['# 저자 구현 기반 비교 결과','',
           '동일 Rust MQTT broker/client 구현에서 TCP+TLS와 QUIC 스트림 배치만 변경했다. '
           '인증서 검증을 켜고 client/broker의 정책을 맞췄다. loopback 결과이며 ESP32·손실·핸드오프 우위는 평가하지 않는다.','',
           '| 메시지 B | QoS | 방식 | 성공/실행 | unique 수신/송신 | 실행별 p95 평균 ms | p99 평균 ms | broker CPU ms | broker peak RSS MiB |',
           '|---:|---:|---|---:|---:|---:|---:|---:|---:|']
    for (payload,qos,case),rows in sorted(groups.items()):
        ok=[r for r in rows if r['status']=='ok']
        summary=dict(payload_bytes=payload,qos=qos,case=case,completed=len(ok),attempted=len(rows),
                     sent=sum(r.get('published',0) for r in rows),received_unique=sum(r['received_unique'] for r in rows),
                     duplicates=sum(r['duplicates'] for r in rows),
                     per_topic_order_violations=sum(r['per_topic_order_violations'] for r in rows),
                     mean_run_p95_ms=mean(ok,'latency_p95_us',.001),mean_run_p99_ms=mean(ok,'latency_p99_us',.001),
                     broker_cpu_ms=mean(ok,'broker_cpu_window_s',1000),
                     broker_peak_rss_mib=mean(ok,'broker_sampled_peak_rss_bytes',1/1024**2))
        aggregates.append(summary)
        def fmt(value):return f'{value:.3f}' if value is not None else '미측정'
        lines.append(f'| {payload} | {qos} | {case} | {len(ok)}/{len(rows)} | '
                     f'{summary["received_unique"]}/{summary["sent"]} | '
                     +' | '.join(fmt(summary[k]) for k in ['mean_run_p95_ms','mean_run_p99_ms','broker_cpu_ms','broker_peak_rss_mib'])+' |')
    lines+=['','callback trace에서 토픽 내 sequence 역전 관측: '+', '.join(
            f'{r["case"]}/QoS{r["qos"]}/{r["payload_bytes"]}B={r["per_topic_order_violations"]}'
            for r in aggregates if r['per_topic_order_violations']) if any(r['per_topic_order_violations'] for r in aggregates)
            else 'callback trace에서 토픽 내 sequence 역전은 관측되지 않았다.', '',
            f'수신 sequence 중복 관측: {sum(r["duplicates"] for r in aggregates)}개.', '',
            '지연은 trace의 unique 메시지로 실행별 계산 후 평균했다. 원본 callback.rs가 각 callback을 tokio::spawn으로 실행하므로 지연에는 callback 스케줄링이 포함되며, trace 역전을 네트워크 전달 순서 위반으로 단정할 수 없다. '
            '전달 수는 publisher 성공 카운터와 수신 sequence를 대조했다. '
            '명목 입력률은 publish 후 sleep 방식이므로 실제 생성률은 정책에 따라 달라질 수 있다. '
            'RAM은 50ms 간격 RSS 표본의 최대이며 Go TotalAlloc/ESP32 heap과 다른 지표다. '
            'broker CPU 관측에는 client 연결·warmup·종료 대기가 포함된다. '
            'CPU 0ms는 누적 CPU tick 증가를 관측하지 못했다는 뜻이다. '
            '고부하에서 송신/수신 차이는 500ms drain 종료 시 미도착 수이며, 확정 패킷 손실과 다르다.','',
            '최적 방식 판정은 전달률·오류·메모리 예산을 먼저 만족해야 한다. '
            '그 후 deadline 충족률/지연과 CPU 비용을 평가한다. '
            '이번에는 네트워크 악화·실제 ESP32 자원 제한을 걸지 않았으므로 전체 우승자를 정하지 않는다.', '',
            f'원자료: `{args.directory}`. 각 실행의 명령, 로그, 메시지 trace, QUIC 통계, 자원 표본과 manifest를 보존했다.','']
    (args.directory/'normalized.json').write_text(json.dumps(data,indent=2)+'\n')
    (args.directory/'aggregate.json').write_text(json.dumps(aggregates,indent=2)+'\n')
    with args.output.open('x') as stream:stream.write('\n'.join(lines))


if __name__=='__main__':
    main()
