#!/usr/bin/env python3
"""Continue only after the separately logged recovery diagnostic succeeds."""
import json
from pathlib import Path
import subprocess
import sys
import time
BASE=Path(__file__).resolve().parent
ROOT=BASE.parents[1]
recovery=BASE/'results/wifi-recovery-v28/manifest.json'
for _ in range(60):
    data=json.loads(recovery.read_text()) if recovery.exists() else {}
    if data.get('status') in ('complete','failed'):
        break
    time.sleep(10)
else:
    raise SystemExit('Recovery diagnostic not complete within 10 minutes; no trials launched')
if data.get('status')!='complete' or not data.get('restore_verified'):
    raise SystemExit('Recovery diagnostic failed; no trials launched')
name='wifi-quality-v28'
command=[str(BASE/'vendor/venv/bin/python'),'-u',str(BASE/'ladder_suite.py'),
         '--name',name,'--policies','low','medium','ewma','--networks','good','medium','poor',
         '--repeats','3','--seed','20260924','--firmware-root',str(BASE/'results/literature-v25-firmware'),'--execute']
suite=subprocess.Popen(command,cwd=ROOT)
manifest=BASE/'results'/name/'manifest.json'
while suite.poll() is None and not manifest.exists():time.sleep(1)
watcher=None
if manifest.exists():
    with (manifest.parent/'report-watcher.log').open('w') as log:
        watcher=subprocess.Popen([sys.executable,str(BASE/'watch_quality_results.py'),name],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
code=suite.wait()
if watcher is not None:
    try:watcher.wait(timeout=60)
    except subprocess.TimeoutExpired:watcher.terminate();watcher.wait(timeout=10)
raise SystemExit(code)
