"""Recompute local AMR evidence; never infer unmeasured scheduler performance."""
import argparse
import csv
import hashlib
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def receive_stats(path):
    if not path.exists():
        return None
    data = rows(path)
    by_type = defaultdict(list)
    for row in data:
        by_type[row.get('type', 'unknown')].append(int(row['t_us']))
    result = {}
    for kind, times in by_type.items():
        gaps = [(b-a)/1e6 for a, b in zip(times, times[1:])]
        duration = (times[-1]-times[0])/1e6 if len(times) > 1 else 0
        result[kind] = dict(frames=len(times), interval_fps=(len(times)-1)/duration if duration else None,
                            gap_over_500ms=sum(g > .5 for g in gaps),
                            max_gap_s=max(gaps, default=0))
    if data:
        duration = (int(data[-1]['t_us'])-int(data[0]['t_us']))/1e6
        result['combined'] = dict(frames=len(data), bytes=sum(int(r['bytes']) for r in data),
                                  goodput_mbps=sum(int(r['bytes']) for r in data)*8/duration/1e6 if duration else None)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mpquic', type=Path, default=Path('/home/ryzen395/mpquic'))
    parser.add_argument('--amr', type=Path, default=Path('/home/ryzen395/slam-mpquic-exp'))
    args = parser.parse_args()
    out = HERE / 'results'
    out.mkdir(exist_ok=True)
    source = args.mpquic / 'results/stationary_20260720/master_summary.csv'
    data = rows(source)
    (out / 'stationary_original.csv').write_bytes(source.read_bytes())
    groups = defaultdict(list)
    evidence = []
    for row in data:
        mode = row['mode']
        groups[mode].append(row)
        directory = source.parent / f"{mode}_{row['run']}"
        log = (directory / 'client/client.log').read_text(errors='replace')
        reasons = Counter(re.findall(r'\[SEL\].*?reason=(\S+)', log))
        receiver = receive_stats(directory / 'server_rx.csv')
        if receiver:
            assert receiver['combined']['frames'] == int(row['rx_frames']), directory
            assert receiver['combined']['bytes'] == int(row['rx_bytes']), directory
            assert abs(receiver['combined']['goodput_mbps']-float(row['goodput_mbps'])) < .02, directory
        evidence.append(dict(mode=mode, run=int(row['run']), path=str(directory),
                             logged_reasons=dict(reasons),
                             planned_duration_exit='[EXP] duration elapsed' in log,
                             receiver=receiver))
    summary = []
    for mode, group in groups.items():
        goodput = [float(r['goodput_mbps']) for r in group]
        summary.append(dict(mode=mode, runs=len(group), goodput_mean=statistics.mean(goodput),
                            goodput_sample_sd=statistics.stdev(goodput) if len(group)>1 else None,
                            switches_mean=statistics.mean(int(r['switches']) for r in group),
                            client_exit_codes=dict(Counter(r['client_exit'] for r in group)),
                            implementation='stub_primary_only' if mode in ('ecf','blest','tof') else 'implemented'))
    mobile = []
    for meta in sorted((args.amr / 'results').glob('*/client/qlogs_client/*_meta.csv')):
        entries = rows(meta)
        if not entries:
            continue
        mode = entries[0].get('scheduler_mode', 'unknown')
        events = rows(meta.with_name(meta.name.replace('_meta.csv', '_events.csv')))
        valid = []
        by_path = Counter()
        for event in events:
            try:
                nbytes = int(event['frame_bytes'])
                if event['choice_reason'] is None or event['path_i'] is None:
                    continue
            except (ValueError, TypeError, KeyError):
                continue
            by_path[event['path_i']] += nbytes
            valid.append(event)
        reasons = Counter(r['choice_reason'] for r in valid)
        directory = meta.parents[2]
        mobile.append(dict(run=directory.name, mode=mode, meta=str(meta), events=len(valid),
                           malformed_events=len(events)-len(valid),
                           reasons=dict(reasons), sent_bytes_by_path=dict(by_path),
                           primary_only_pct=100*reasons['primary_only']/len(valid) if valid else None,
                           server_anchor=(directory / 'server_meta.csv').exists(),
                           pose=(directory / 'client/pose.csv').exists(),
                           receiver=receive_stats(directory / 'server_rx.csv')))
    result = dict(stationary_source=str(source), source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  stationary_summary=summary, stationary_evidence=evidence, amr_runs=mobile,
                  limitations=['Stationary runs do not test mobility or ESP32 RAM pressure.',
                               'RGB and depth frames are separate; aggregate frame count is not camera FPS.',
                               'Logged selector messages are not per-frame decisions.',
                               'AMR run folder names do not establish motion, route or firmware identity.',
                               'No source-to-historical-binary hash binding was found.'])
    (out / 'analysis.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'stationary':summary,'amr_modes':dict(Counter(r['mode'] for r in mobile)),
                      'amr_runs':len(mobile),'planned_exits':sum(r['planned_duration_exit'] for r in evidence)}, indent=2))


if __name__ == '__main__':
    main()
