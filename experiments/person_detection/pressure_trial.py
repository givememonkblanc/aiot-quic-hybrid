"""Bounded live FIFO/adaptive comparison; requested FPS is not measured FPS.

Uses the existing USB console without changing Wi-Fi settings or saving NVS.
Restores adaptive mode and the ordinary queue budget even when a trial fails.
"""
import argparse
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read_json(path):
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/ttyACM0')
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--budget-kib', type=int, default=6144)
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not (1 <= args.seconds <= 120 and 64 <= args.budget_kib <= 6144 and 1 <= args.fps <= 30):
        parser.error('seconds 1..120, budget 64..6144 KiB, target FPS 1..30')
    args.output.mkdir(parents=True, exist_ok=False)
    memory = ROOT/'experiments/memory/results/latest.json'
    rates = ROOT/'experiments/person_detection/results/rates.json'
    initial = read_json(memory)
    if 'queue_budget' not in initial:
        raise RuntimeError('Firmware with queue accounting has not reported yet')
    previous_uptime = initial['uptime_us']
    fd = os.open(args.port, os.O_WRONLY | os.O_NOCTTY)
    outcome = {'requested_fps': args.fps, 'budget_kib': args.budget_kib,
               'note': 'Live camera input, not synthetic/repeated frames; diagnostic heap guards enabled',
               'started_utc': datetime.now(timezone.utc).isoformat()}
    try:
        with (args.output/'samples.jsonl').open('w') as log:
            for mode in (1, 2):
                os.write(fd, f'\nvideo {args.fps} {args.budget_kib}\nmode {mode}\n'.encode())
                start = time.monotonic()
                print(f'Start mode={mode} target_fps={args.fps} budget_kib={args.budget_kib}', flush=True)
                while time.monotonic()-start < args.seconds:
                    row = read_json(memory)
                    uptime = row['uptime_us']
                    age = time.time()-datetime.fromisoformat(row['received_utc']).timestamp()
                    if uptime < previous_uptime:
                        raise RuntimeError('Device rebooted: stop, do not mix runs')
                    if age > 25:
                        raise RuntimeError('RAM telemetry stalled for over 25 seconds')
                    if row['internal_free'] < 16384:
                        raise RuntimeError('Internal free heap reached 16 KiB stop threshold')
                    previous_uptime = uptime
                    sample = dict(epoch=time.time(), requested_mode=mode, phase_s=time.monotonic()-start,
                                  ram=row, rates=read_json(rates))
                    log.write(json.dumps(sample)+'\n'); log.flush()
                    time.sleep(1)
        outcome['status'] = 'completed'
    except Exception as exc:
        outcome.update(status='stopped', reason=str(exc))
        raise
    finally:
        os.write(fd, b'\nmode 2\nvideo 15 256\n'); os.close(fd)
        outcome['restore_command'] = 'mode 2; video 15 256 (runtime only)'
        (args.output/'outcome.json').write_text(json.dumps(outcome, indent=2)+'\n')
        print(json.dumps(outcome), flush=True)


if __name__ == '__main__':
    main()
