#!/usr/bin/env python3
"""Bounded read-only suite monitor; writes reports, never touches the device."""
import json
from pathlib import Path
import subprocess
import sys
import time
BASE = Path(__file__).resolve().parent
suite = BASE / 'results' / (sys.argv[1] if len(sys.argv) > 1 else 'wifi-quality-v27')
limit = time.monotonic() + 3 * 3600
last = None
while time.monotonic() < limit:
    manifest = json.loads((suite / 'manifest.json').read_text())
    done = sum(r['status'] != 'running' for r in manifest['results'])
    state = (done, manifest['status'])
    if done and state != last:
        subprocess.run([sys.executable, str(BASE / 'analyze_ladder.py'), str(suite), '--plot'], check=False)
        last = state
    if manifest['status'] in ('complete', 'stopped'):
        print(json.dumps({'status': manifest['status'], 'finished_runs': done}), flush=True)
        break
    time.sleep(20)
else:
    raise SystemExit('Report watcher timed out; suite is independently managed')
