#!/usr/bin/env python3
"""Analyze picoquic draft-00 qlog; never count retransmitted bytes as goodput."""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path


class Ranges:
    def __init__(self):
        self.ranges = []
        self.total = 0
        self.frontier = 0

    def add(self, start, length):
        if start < 0 or length < 0:
            raise ValueError('negative STREAM offset/length')
        if not length:
            return 0, 0
        end = start + length
        merged = []
        for lo, hi in self.ranges:
            if hi < start:
                merged.append((lo, hi))
            elif end < lo:
                merged.append((start, end))
                start, end = lo, hi
            else:
                start, end = min(start, lo), max(end, hi)
        merged.append((start, end))
        total = sum(hi-lo for lo, hi in merged)
        frontier = merged[0][1] if merged[0][0] == 0 else 0
        new, delivered = total-self.total, frontier-self.frontier
        self.ranges, self.total, self.frontier = merged, total, frontier
        return new, delivered


def percentile(values, p):
    if not values:
        return None
    values = sorted(values)
    pos = (len(values)-1)*p
    lo = int(pos)
    return values[lo] + (values[min(lo+1, len(values)-1)]-values[lo])*(pos-lo)


def analyze(qlog, manifest):
    doc = json.loads(Path(qlog).read_text())
    traces = doc['traces']
    if len(traces) != 1 or traces[0]['vantage_point']['type'] != 'client':
        raise ValueError('expected exactly one client trace')
    trace = traces[0]
    unit = trace.get('configuration', {}).get('time_units')
    if unit not in ('us', 'ms', 's'):
        raise ValueError(f'unsupported qlog time unit: {unit}')
    scale = {'us': 1e-6, 'ms': 1e-3, 's': 1}[unit]
    ref = float(trace['common_fields']['reference_time'])*scale
    if manifest.get('clock') != 'CLOCK_MONOTONIC':
        raise ValueError('manifest must explicitly use CLOCK_MONOTONIC')
    if abs(ref-manifest['client_start_monotonic_s']) > 5:
        raise ValueError('qlog and runner clocks differ or client startup exceeded 5 seconds')
    streams = defaultdict(Ranges)
    paths = defaultdict(lambda: {'received_packets': 0, 'received_wire_bytes': 0,
                                'unique_stream_bytes': 0, 'rtt_samples_ms': []})
    progress, rows, fins = [], [], {}
    first = None
    for raw in trace['events']:
        e = dict(zip(trace['event_fields'], raw))
        t = float(e['relative_time'])*scale
        path = str(e.get('path_id', 0))
        data = e['data']
        if e['event'] == 'metrics_updated' and 'smoothed_rtt' in data:
            paths[path]['rtt_samples_ms'].append(float(data['smoothed_rtt'])*scale*1000)
        if e['event'] != 'packet_received':
            continue
        paths[path]['received_packets'] += 1
        paths[path]['received_wire_bytes'] += int(data.get('header', {}).get('packet_size', 0))
        new = delivered = 0
        for f in data.get('frames', []):
            if f.get('frame_type') != 'stream':
                continue
            sid = int(f.get('id', f.get('stream_id', 0)))
            offset, length = int(f.get('offset', 0)), int(f.get('length', 0))
            n, d = streams[sid].add(offset, length)
            new += n
            delivered += d
            if f.get('fin'):
                fins[sid] = offset + length
        paths[path]['unique_stream_bytes'] += new
        if new and first is None:
            first = t
        if delivered:
            progress.append((t, delivered))
        if new or delivered:
            rows.append({'relative_s': t, 'monotonic_s': ref+t, 'path_id': path,
                         'unique_bytes': new, 'in_order_bytes': delivered})
    last = progress[-1][0] if progress else None
    gaps = [b[0]-a[0] for a, b in zip(progress, progress[1:])]
    event_metrics = []
    for e in manifest.get('events', []):
        if e.get('kind') != 'impairment':
            continue
        # Linux picoquic uses CLOCK_MONOTONIC; never mix this with Unix time.
        boundary = e['completed_monotonic_s']-ref
        after = next((t for t, _ in progress if t >= boundary), None)
        before = next((t for t, _ in reversed(progress) if t < boundary), None)
        event_metrics.append({'name': e['name'], 'relative_s': boundary,
            'apply_duration_ms': (e['completed_monotonic_s']-e['started_monotonic_s'])*1000,
            'next_in_order_progress_ms': None if after is None else (after-boundary)*1000,
            'straddling_progress_gap_ms': None if before is None or after is None else (after-before)*1000})
    total = sum(s.total for s in streams.values())
    for p in paths.values():
        samples = p.pop('rtt_samples_ms')
        p['rtt_sample_count'] = len(samples)
        p['rtt_p50_ms'] = percentile(samples, .5)
        p['rtt_p95_ms'] = percentile(samples, .95)
    active = sum(p['unique_stream_bytes'] > 0 for p in paths.values())
    complete = bool(streams) and all(sid in fins and s.frontier == fins[sid] for sid, s in streams.items())
    negotiated = 'Enable multipath: Success.' in manifest.get('client_log', '')
    reasons = []
    if manifest.get('client_returncode') != 0:
        reasons.append('client_failed_or_timed_out')
    if not complete or total != manifest['bytes']:
        reasons.append('incomplete_or_unexpected_transfer')
    if manifest['mode'] == 'multipath' and (not negotiated or active < 2):
        reasons.append('multipath_not_confirmed_on_two_data_paths')
    if len(event_metrics) != manifest.get('expected_events', 0):
        reasons.append('scenario_not_fully_applied')
    if any(e['next_in_order_progress_ms'] is None for e in event_metrics):
        reasons.append('no_progress_after_scenario_event')
    summary = {'mode': manifest['mode'], 'scenario': manifest['scenario'],
        'valid': not reasons, 'invalid_reasons': reasons, 'multipath_negotiated': negotiated,
        'data_path_count': active, 'unique_stream_bytes': total,
        'transfer_complete': complete, 'time_to_last_in_order_byte_s': last,
        'goodput_mbps_from_connection_start': total*8/last/1e6 if last else None,
        'first_stream_byte_s': first, 'max_in_order_progress_gap_ms': max(gaps)*1000 if gaps else None,
        'progress_gap_p95_ms': percentile(gaps, .95)*1000 if gaps else None,
        'paths': dict(paths), 'events': event_metrics,
        'measurement': 'receiver STREAM reassembly proxy, not application callback latency'}
    return summary, rows


def analyze_run(folder):
    folder = Path(folder)
    manifest = json.loads((folder/'manifest.json').read_text())
    manifest['client_log'] = (folder/'client.log').read_text(errors='replace')
    qlogs = list((folder/'client_qlog').glob('*.qlog'))
    try:
        if len(qlogs) != 1:
            raise ValueError(f'expected one client qlog, got {len(qlogs)}')
        summary, rows = analyze(qlogs[0], manifest)
    except (ValueError, KeyError) as exc:
        summary = {'mode': manifest['mode'], 'scenario': manifest['scenario'],
            'valid': False, 'invalid_reasons': ['missing_or_unreadable_qlog'],
            'analysis_error': str(exc), 'client_returncode': manifest.get('client_returncode')}
        if manifest.get('client_returncode') != 0:
            summary['invalid_reasons'].append('client_failed_or_timed_out')
        rows = []
    (folder/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    with (folder/'progress.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=['relative_s', 'monotonic_s', 'path_id', 'unique_bytes', 'in_order_bytes'])
        writer.writeheader()
        writer.writerows(rows)
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    result = analyze_run(args.run)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['valid'] else 2)
