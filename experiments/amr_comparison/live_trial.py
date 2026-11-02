"""Bounded on-device policy crossover at fixed input/queue budget; no NVS writes."""
import argparse
import json
import os
import statistics
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--seconds', type=int, default=30)
    args = parser.parse_args()
    if not 20 <= args.seconds <= 60:
        parser.error('seconds must be 20..60')
    memory = ROOT/'experiments/memory/results/latest.json'
    rates = ROOT/'experiments/person_detection/results/rates.json'
    initial = read(memory)
    if 'video_policy' not in initial:
        raise RuntimeError('Ported firmware telemetry not yet received')
    args.output.mkdir(parents=True, exist_ok=False)
    previous_uptime = initial['uptime_us']
    # Reverse the order on repetition. These are live scenes, not identical traces.
    stages = [(1, 0), (2, 0), (2, 1), (2, 2), (2, 3),
              (2, 3), (2, 2), (2, 1), (2, 0), (1, 0)]
    samples = []
    outcome = dict(started_utc=datetime.now(timezone.utc).isoformat(), target_fps=30,
                   budget_kib=6144, seconds_per_stage=args.seconds)
    fd = os.open('/dev/ttyACM0', os.O_WRONLY | os.O_NOCTTY)
    try:
        with (args.output/'samples.jsonl').open('w') as log:
            for stage, (mode, policy) in enumerate(stages):
                os.write(fd, f'\nvideo 30 6144\npolicy {policy}\nmode {mode}\n'.encode())
                start = time.monotonic()
                print(f'stage={stage} mode={mode} policy={policy}', flush=True)
                matched = False
                while time.monotonic()-start < args.seconds:
                    ram = read(memory)
                    age = time.time()-datetime.fromisoformat(ram['received_utc']).timestamp()
                    if ram['uptime_us'] < previous_uptime:
                        raise RuntimeError('Device rebooted')
                    if age > 25 or ram['internal_free'] < 16384:
                        raise RuntimeError('Telemetry stalled or internal free heap below 16KiB')
                    previous_uptime = ram['uptime_us']
                    phase = time.monotonic()-start
                    match = ram['mode']==mode and ram.get('video_policy')==policy and ram['target_fps']==30 and ram['queue_budget']==6291456
                    matched |= match
                    if phase > 20 and not matched:
                        raise RuntimeError('Requested policy not acknowledged in telemetry')
                    sample = dict(stage=stage, requested_mode=mode, requested_policy=policy,
                                  phase_s=phase, epoch=time.time(), matched=match, ram=ram, rates=read(rates))
                    samples.append(sample); log.write(json.dumps(sample)+'\n'); log.flush()
                    time.sleep(1)
        outcome['status'] = 'completed'
    except Exception as exc:
        outcome.update(status='stopped', reason=str(exc))
        raise
    finally:
        os.write(fd, b'\nmode 2\npolicy 0\nvideo 30 6144\n'); os.close(fd)
        outcome['restored'] = 'mode 2, policy 0, video 30 6144'
        (args.output/'outcome.json').write_text(json.dumps(outcome, indent=2)+'\n')
        summary = []
        for stage, (mode, policy) in enumerate(stages):
            rows = [s for s in samples if s['stage']==stage and s['matched']]
            steady = [s for s in rows if s['phase_s']>=10]
            if not rows or not steady:
                continue
            first, last = rows[0]['ram'], rows[-1]['ram']
            delta = last['uptime_us']-first['uptime_us']
            summary.append(dict(stage=stage, mode=mode, policy=policy,
                psram_peak_pct=max(s['ram']['psram_used_pct'] for s in rows),
                queue_peak_bytes=max(s['ram']['queue_bytes'] for s in rows),
                internal_min_free=min(s['ram']['internal_free'] for s in rows),
                capture_fps=(last['frames']-first['frames'])*1e6/delta if delta else None,
                received_fps=statistics.mean(s['rates']['valid_frame_fps_10s'] for s in steady),
                ai_result_fps=statistics.mean(s['rates']['ai_result_fps_10s'] for s in steady)))
        (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
        print(json.dumps(outcome), flush=True)


if __name__ == '__main__':
    main()
