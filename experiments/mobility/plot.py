#!/usr/bin/env python3
"""Export repeat-level comparison and representative receiver progress as PNG/PDF."""
import argparse
import csv
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('matrix', type=Path)
    args = p.parse_args()
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    doc = json.loads((args.matrix/'aggregate.json').read_text())
    scenarios = sorted({g['scenario'] for g in doc['groups']})
    colors = {'single': '#64748b', 'multipath': '#007f86'}
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), constrained_layout=True)
    for ax, metric, label in zip(axes,
        ['goodput_mbps_from_connection_start', 'max_in_order_progress_gap_ms'],
        ['Goodput from connection start (Mbps)', 'Maximum in-order progress gap (ms)']):
        for mode, offset in [('single', -.17), ('multipath', .17)]:
            for i, scenario in enumerate(scenarios):
                values = [t[metric] for t in doc['trials'] if t['valid'] and t['scenario'] == scenario and t['mode'] == mode]
                if not values:
                    continue
                import statistics
                ax.errorbar(i+offset, statistics.mean(values),
                    yerr=statistics.stdev(values) if len(values)>1 else 0,
                    fmt='o', capsize=4, color=colors[mode], label=mode if i == 0 else None)
                jitter = [(j-(len(values)-1)/2)*.025 for j in range(len(values))]
                ax.scatter([i+offset+j for j in jitter], values, alpha=.5, s=16, color=colors[mode])
        ax.set_xticks(range(len(scenarios)), scenarios)
        ax.set_ylabel(label)
        ax.set_ylim(bottom=0)
        ax.grid(axis='y', alpha=.2)
        ax.legend()
    fig.suptitle('Linux two-path emulation: individual trials and mean ± sample SD')
    for ext in ('png', 'pdf'):
        fig.savefig(args.matrix/f'comparison.{ext}', dpi=160)
    plt.close(fig)
    fig, axes = plt.subplots(len(scenarios), 1, figsize=(10, 3*len(scenarios)), squeeze=False, constrained_layout=True)
    for ax, scenario in zip(axes[:, 0], scenarios):
        for mode in ('single', 'multipath'):
            # First valid run: deterministic choice, not the best-looking trial.
            trial = next((t for t in doc['trials'] if t['valid'] and t['scenario'] == scenario and t['mode'] == mode), None)
            if trial is None:
                continue
            folder = args.matrix/trial['run']
            with (folder/'progress.csv').open() as f:
                rows = list(csv.DictReader(f))
            times, values, total = [0.], [0.], 0
            for row in rows:
                total += int(row['in_order_bytes'])
                times.append(float(row['relative_s']))
                values.append(total/(1024*1024))
            ax.step(times, values, where='post', color=colors[mode], label=trial['run'])
            for e in trial['events']:
                ax.axvline(e['relative_s'], color=colors[mode], alpha=.25, linestyle='--')
        ax.set_title(scenario)
        ax.set_ylabel('Contiguous STREAM data (MiB)')
        ax.set_xlabel('Seconds since QUIC connection creation')
        ax.grid(alpha=.2)
        ax.legend(fontsize=8)
    fig.suptitle('Receiver reassembly progress; dashed lines: impairment changes')
    for ext in ('png', 'pdf'):
        fig.savefig(args.matrix/f'progress.{ext}', dpi=160)
    plt.close(fig)


if __name__ == '__main__':
    main()
