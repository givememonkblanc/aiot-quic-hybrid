#!/usr/bin/env python3
"""Run sequential paired trials; retain failures and aggregate valid trials only."""
import argparse
import csv
import json
import random
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
METRICS = ['goodput_mbps_from_connection_start', 'time_to_last_in_order_byte_s',
           'max_in_order_progress_gap_ms']


def aggregate(out):
    trials = []
    for folder in sorted(out.glob('*_r*')):
        if not folder.is_dir():
            continue
        m = json.loads((folder/'manifest.json').read_text())
        summary = folder/'summary.json'
        s = json.loads(summary.read_text()) if summary.exists() else {'valid': False,
            'invalid_reasons': [m.get('error') or f"summary_missing (client_returncode={m.get('client_returncode')})"]}
        trials.append({'run': folder.name, 'mode': m['mode'], 'scenario': m['scenario'], **s})
    groups = []
    for scenario, mode in sorted({(r['scenario'], r['mode']) for r in trials}):
        selected = [r for r in trials if r['scenario'] == scenario and r['mode'] == mode]
        valid = [r for r in selected if r['valid']]
        g = {'scenario': scenario, 'mode': mode, 'total_runs': len(selected), 'valid_runs': len(valid)}
        for metric in METRICS:
            values = [r[metric] for r in valid if r.get(metric) is not None]
            g[metric+'_mean'] = statistics.mean(values) if values else None
            g[metric+'_std'] = statistics.stdev(values) if len(values) > 1 else None
        groups.append(g)
    (out/'aggregate.json').write_text(json.dumps({'groups': groups, 'trials': trials}, indent=2)+'\n')
    if groups:
        with (out/'aggregate.csv').open('w') as f:
            w = csv.DictWriter(f, fieldnames=list(groups[0]))
            w.writeheader()
            w.writerows(groups)
    lines = ['# MP-QUIC mobility measurements', '',
        'Linux netem emulation; downlink bulk transfer. Values are mean ± sample SD.',
        'In-order progress is reconstructed from STREAM offsets, not application timestamps.', '',
        '| Scenario | Mode | Valid / total | Goodput (Mbps) | Completion (s) | Max progress gap (ms) |',
        '|---|---|---:|---:|---:|---:|']
    def fmt(g, metric):
        mean, std = g[metric+'_mean'], g[metric+'_std']
        return '—' if mean is None else f'{mean:.3f}' + (f' ± {std:.3f}' if std is not None else '')
    for g in groups:
        cells = [g['scenario'], g['mode'], f"{g['valid_runs']} / {g['total_runs']}"]
        cells += [fmt(g, metric) for metric in METRICS]
        lines.append('| '+' | '.join(cells)+' |')
    (out/'report.md').write_text('\n'.join(lines)+'\n')
    return all(r['valid'] for r in trials) and bool(trials)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--scenarios', nargs='+', choices=['static', 'outage', 'handover'], default=['static', 'outage', 'handover'])
    p.add_argument('--bytes', type=int, default=32*1024*1024)
    p.add_argument('--time-scale', type=float, default=1)
    p.add_argument('--timeout', type=float, default=180)
    p.add_argument('--seed', type=int, default=20260920)
    p.add_argument('--analyze-only', action='store_true')
    args = p.parse_args()
    if args.analyze_only:
        return 0 if aggregate(args.out) else 2
    if args.repeats < 1:
        p.error('repeats must be positive')
    args.out.mkdir(parents=True, exist_ok=False)
    rng = random.Random(args.seed)
    schedule = []
    for rep in range(1, args.repeats+1):
        scenarios = list(args.scenarios)
        rng.shuffle(scenarios)
        for scenario in scenarios:
            modes = ['single', 'multipath']
            rng.shuffle(modes)
            schedule.extend((scenario, mode, rep) for mode in modes)
    (args.out/'schedule.json').write_text(json.dumps({'seed': args.seed, 'schedule': schedule,
        'note': 'seed controls trial order, not kernel netem loss or QUIC random numbers'}, indent=2)+'\n')
    failed = False
    for i, (scenario, mode, rep) in enumerate(schedule, 1):
        folder = args.out/f'{scenario}_{mode}_r{rep}'
        print(f'[{i}/{len(schedule)}] {folder.name}', flush=True)
        command = [sys.executable, str(HERE/'run.py'), '--mode', mode, '--scenario', scenario,
            '--out', str(folder), '--bytes', str(args.bytes), '--time-scale', str(args.time_scale),
            '--timeout', str(args.timeout)]
        result = subprocess.run(command, capture_output=True, text=True)
        (args.out/f'{folder.name}.runner.log').write_text(result.stdout+result.stderr)
        if result.returncode:
            failed = True
            print(f'  FAILED (exit {result.returncode}); retained logs', flush=True)
        if not folder.joinpath('manifest.json').exists():
            raise RuntimeError('runner failed before writing manifest; inspect runner log')
        aggregate(args.out)
    return 2 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
