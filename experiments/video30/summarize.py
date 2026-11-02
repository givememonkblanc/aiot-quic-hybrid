"""Summarize timing probes alongside receiver-validated JPEG goodput."""
import json
import subprocess
import sys
from pathlib import Path

run = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[2]
subprocess.run([sys.executable, str(root / 'experiments/vlm_split/analyze_integrated.py'), str(run)], check=True, stdout=subprocess.DEVNULL)
rows = [json.loads(line) for line in (run / 'records.jsonl').read_text().splitlines()]
meta = json.loads((run / 'outcome.json').read_text())
summaries = json.loads((run / 'summary.json').read_text())
for summary in summaries:
    stage = next(s for s in meta['stages'] if s['index'] == summary['stage'])
    lo, hi = stage['start'] + meta['settle_seconds'], stage['end']
    perf = [r['data'] for r in rows if r['tag'] == 'DGRAM_PERF' and lo <= r['host'] < hi]
    totals = {k: sum(p[k] for p in perf) for k in ['calls', 'ok', 'refused', 'bytes', 'lock_us', 'encode_us', 'udp_us', 'total_us']}
    summary['datagram_timing'] = totals
    summary['completed_preview_fps'] = summary['complete_preview'] / summary['window_s']
    summary['transport_valid'] = bool(summary['mqtt_received'] or summary['complete_preview'] or summary['complete_roi'])
    if totals['calls']:
        summary['datagram_mean_us'] = {k: totals[k] / totals['calls'] for k in ['lock_us', 'encode_us', 'udp_us', 'total_us']}
(run / 'video30-summary.json').write_text(json.dumps(summaries, indent=2) + '\n')
for s in summaries:
    print(json.dumps({k: s.get(k) for k in ['name', 'transport_valid', 'input_fps', 'completed_preview_fps', 'complete_KiBs', 'ai_ms_mean', 'min_internal_free', 'queue_peak', 'datagram_mean_us']}))
