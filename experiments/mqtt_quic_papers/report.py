"""Build a reviewable paper-KPI comparison from separate timing/allocation passes."""
import argparse
import json
from pathlib import Path
import statistics


def load(directory, expected):
    rows = json.loads((directory / 'summary.json').read_text())
    if any(r.get('measurement_pass') != expected for r in rows):
        raise ValueError(f'{directory}: expected a pure {expected} pass')
    signatures = {(r['sent'], r['payload_bytes'], r['interval_ms']) for r in rows}
    if len(signatures) != 1:
        raise ValueError('Mixed workloads cannot be aggregated')
    return rows, signatures.pop()


def mean_sd(rows, field, scale=1):
    values = [r[field]*scale for r in rows if r.get(field) is not None]
    if not values:
        return '미측정'
    mean = statistics.mean(values)
    sd = statistics.stdev(values) if len(values)>1 else 0
    return f'{mean:.3f} ± {sd:.3f}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timing', required=True, type=Path)
    parser.add_argument('--allocation', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    timing, signature = load(args.timing, 'timing')
    allocation, other = load(args.allocation, 'allocation')
    if signature != other:
        raise ValueError('Timing/allocation workloads must match')
    lines = ['# 논문 KPI 정의를 적용한 호스트 비교', '',
             '논문 원본 구현·하드웨어·네트워크의 재현 결과가 아니다. MQTT 3.1.1 QoS0/1, '
             'Mosquitto 공통 broker, TLS 1.3/TCP 대 aioquic STREAM의 loopback 실험이다.', '',
             f'부하: {signature[0]}개 × {signature[1]}B, {signature[2]}ms 간격. '
             'Saif Table VII와 Jeddou의 지연 분포/RSD 정의를 참조했다. '
             'Saif의 H3 비교군과 NB-IoT emulation은 포함하지 않았다.', '',
             '표는 실행별 지표의 평균 ± 실행 간 표본 표준편차다. '
             'RSD는 각 실행 메시지의 population SD/mean(무차원)이다. '
             'raw JSON에는 sample SD 버전도 있다.', '',
             '| 전송 | QoS | 반복 | 수신/송신 | 지연 평균 ms | p95 ms | RSD | 실행 ms | CPU ms |',
             '|---|---:|---:|---:|---:|---:|---:|---:|---:|']
    for transport in ('tcp-tls', 'quic-stream'):
        for qos in (0, 1):
            rows = [r for r in timing if r['transport']==transport and r['qos']==qos]
            if not rows:
                raise ValueError('Missing comparison group')
            values = [mean_sd(rows, key, scale) for key, scale in
                      [('latency_mean_ms',1), ('latency_p95_ms',1), ('latency_rsd_population',1),
                       ('publisher_execution_s',1000), ('cpu_process_s',1000)]]
            lines.append(f'| {transport} | {qos} | {len(rows)} | '
                         f'{sum(r["received_unique"] for r in rows)}/{sum(r["sent"] for r in rows)} | '
                         + ' | '.join(values) + ' |')
    lines += ['', 'CPU 범위는 publisher+subscriber+보안 proxy가 함께 실행되는 Python 프로세스다. '
              'Mosquitto는 제외한다. 논문의 단일 클라이언트 CPU 수치와 직접 대조할 수 없다. '
              '실행 시간은 publisher 연결 API 시작부터 마지막 메시지 전달/PUBACK까지다.', '',
              '## 별도 계측 실행의 누적 메모리 할당', '',
              '| 전송 | QoS | 반복 | Python 요청 누적 KiB | native 요청 누적 KiB |',
              '|---|---:|---:|---:|---:|']
    for transport in ('tcp-tls', 'quic-stream'):
        for qos in (0, 1):
            rows = [r for r in allocation if r['transport']==transport and r['qos']==qos]
            if not rows:
                raise ValueError('Missing allocation comparison group')
            lines.append(f'| {transport} | {qos} | {len(rows)} | '
                         f'{mean_sd(rows, "python_allocation_total_bytes", 1/1024)} | '
                         f'{mean_sd(rows, "native_allocation_total_bytes", 1/1024)} |')
    lines += ['', '두 allocator 계층은 겹치므로 합산하지 않는다. 해제된 메모리의 과거 요청도 '
              '누적되므로 RAM 최고 점유량이 아니다. Go pprof와 도구/언어 범위가 다르다. '
              '메모리 계측 실행의 지연/CPU 수치는 위 성능 표에 섞지 않았다.', '',
              '## 첫 메시지 영향 확인', '',
              '전체 지연 분포에는 새 연결의 첫 메시지도 포함했다. 아래는 원인 분석용 분리이며 '
              '논문의 전체 지연 KPI를 대체하지 않는다. 첫 메시지 지연은 packet 기준 TtFDF와 다르다.', '',
              '| 전송 | QoS | 첫 메시지 지연 평균 ms | 나머지 메시지 지연 평균 ms |',
              '|---|---:|---:|---:|']
    for transport in ('tcp-tls', 'quic-stream'):
        for qos in (0, 1):
            samples = []
            for row in timing:
                if row['transport']==transport and row['qos']==qos:
                    file = args.timing / f'{transport}-qos{qos}-run{row["run"]}.jsonl'
                    samples.extend(json.loads(line) for line in file.read_text().splitlines())
            first = [r['latency_ms'] for r in samples if r['seq']==0]
            rest = [r['latency_ms'] for r in samples if r['seq']!=0]
            rest_value = f'{statistics.mean(rest):.3f}' if rest else '해당 없음'
            lines.append(f'| {transport} | {qos} | {statistics.mean(first):.3f} | {rest_value} |')
    lines += ['', '공통 broker/bridge 경로와 새 연결 효과가 포함된 결과다. '
              '특정 전송 계층만의 효과나 ESP32 이동성 우위를 이 표로 입증하지 않는다.', '',
              '## 미측정 항목', '',
              '- wire bytes/packets, Initial→첫 PUBLISH: 캡처 권한 없음. '
              '`pcap_metrics.py`로 별도 확보한 실제 캡처를 처리할 수 있다.',
              '- 평균 전력/에너지: 전력계 미연결. CPU 시간으로 환산하지 않았다.',
              '- compiled footprint: Python+공유 native 의존성으로 논문의 단일 compiled binary와 불일치.',
              '- ESP32 RAM, 핸드오프 처리량/복구: 이번 실험 대상에 포함되지 않았다.', '',
              '## 자료', '',
              f'- timing: `{args.timing}`', f'- allocation: `{args.allocation}`',
              '- [Saif Table VII](https://arxiv.org/pdf/2208.11178)',
              '- [Jeddou 2022](https://doi.org/10.3390/s22103694)', '']
    with args.output.open('x') as stream:
        stream.write('\n'.join(lines))


if __name__ == '__main__':
    main()
