"""Bounded live queue-policy pilot. Does not measure AI quality or causal joint-control gains."""
import csv
import io
import json
import os
import time
import termios
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RAM = ROOT/'experiments/memory/results/latest.json'
FEED = ROOT/'experiments/handoff/results/cross-ap-final-mode2-03'

def read():
    return json.loads(RAM.read_text())

def fresh(r):
    return time.time()-datetime.fromisoformat(r['received_utc']).timestamp() < 25

def main():
    initial = read()
    if not fresh(initial):
        raise RuntimeError('No fresh device telemetry')
    out = ROOT/'experiments/aiot_resource_management/results'/datetime.now(timezone.utc).strftime('pilot-%Y%m%dT%H%M%SZ')
    out.mkdir(parents=True, exist_ok=False)
    (out/'initial.json').write_text(json.dumps(initial, indent=2))
    sources = [FEED/'ai_recv.csv', FEED/'datagrams.csv']
    offsets = {p: p.stat().st_size for p in sources}
    headers = {p: p.open().readline() for p in sources}
    fd = os.open('/dev/ttyACM0', os.O_WRONLY | os.O_NOCTTY)
    previous = initial['uptime_us']
    outcome = {'status': 'running', 'limitations': ['live scene, not identical sensor replay', 'no ground truth or energy measurement', 'no injected handover', 'queue policy, not full joint resource manager'], 'stages': []}
    rows = []
    try:
        with (out/'samples.jsonl').open('w') as f:
            for index, mode in enumerate([1, 2, 2, 1]):
                os.write(fd, f'\nvideo {initial["target_fps"]} {initial["queue_budget"]//1024}\npolicy {initial["video_policy"]}\nmode {mode}\n'.encode())
                start = time.monotonic()
                outcome['stages'].append({'index': index, 'mode': mode, 'start_monotonic': start})
                print(f'stage {index}: mode={mode}, 45 seconds', flush=True)
                acknowledged = False
                while time.monotonic()-start < 45:
                    r = read()
                    if not fresh(r) or r['internal_free'] < 16384 or r['uptime_us'] < previous:
                        raise RuntimeError('Telemetry/heap/reboot guard stopped trial')
                    previous = r['uptime_us']
                    match = r['mode']==mode and r['video_policy']==initial['video_policy'] and r['queue_budget']==initial['queue_budget'] and r['target_fps']==initial['target_fps']
                    acknowledged |= match
                    if time.monotonic()-start > 25 and not acknowledged:
                        raise RuntimeError('Policy change not acknowledged')
                    row = {'stage':index,'mode':mode,'phase_s':time.monotonic()-start,'host_monotonic':time.monotonic(),'matched':match,'ram':r}
                    rows.append(row)
                    f.write(json.dumps(row)+'\n'); f.flush()
                    time.sleep(1)
                outcome['stages'][-1]['end_monotonic'] = time.monotonic()
        outcome['status'] = 'completed'
    except Exception as exc:
        outcome.update(status='stopped', reason=str(exc))
    finally:
        restore = f'\nmode {initial["mode"]}\npolicy {initial["video_policy"]}\nvideo {initial["target_fps"]} {initial["queue_budget"]//1024}\n'
        os.write(fd, restore.encode()); termios.tcdrain(fd); os.close(fd)
        outcome['restore_requested'] = restore.strip().splitlines()
        for p in sources:
            with p.open('rb') as f:
                f.seek(offsets[p]); data = f.read()
            # Keep only complete appended records.
            data = data[:data.rfind(b'\n')+1]
            (out/p.name).write_bytes(headers[p].encode()+data)
        for _ in range(20):
            r = read()
            if fresh(r) and all(r[k]==initial[k] for k in ('mode','video_policy','target_fps','queue_budget')):
                outcome['restore_confirmed'] = True
                break
            time.sleep(1)
        else:
            outcome['restore_confirmed'] = False
        (out/'outcome.json').write_text(json.dumps(outcome, indent=2))
    ai = list(csv.DictReader((out/'ai_recv.csv').open()))
    summary = []
    for stage in outcome['stages']:
        seen = {}
        for row in rows:
            if row['stage']==stage['index'] and row['matched'] and row['phase_s']>=10:
                seen[(row['ram']['cid'],row['ram']['seq'])] = row['ram']
        rr = list(seen.values())
        if len(rr)<2 or 'end_monotonic' not in stage:
            continue
        first,last = rr[0],rr[-1]
        duration = (last['uptime_us']-first['uptime_us'])/1e6
        a = [x for x in ai if stage['start_monotonic']+10 <= int(x['recv_us'])/1e6 < stage['end_monotonic']]
        summary.append({'stage':stage['index'],'mode':stage['mode'],'unique_ram_samples':len(rr), 'capture_fps':(last['frames']-first['frames'])/duration,'ai_received_count':len(a),'ai_received_fps':len(a)/(stage['end_monotonic']-stage['start_monotonic']-10),'latest_inference_ms_samples':[x['ai_us']/1000 for x in rr], 'internal_min_free':min(x['internal_free'] for x in rr),'internal_min_largest':min(x['internal_largest'] for x in rr),'psram_min_free':min(x['psram_free'] for x in rr),'psram_peak_sampled_used':max(x['psram_total']-x['psram_free'] for x in rr),'queue_peak_sampled':max(x['queue_bytes'] for x in rr),'rtt_ms_samples':[x['quic_rtt_ms'] for x in rr]})
    (out/'summary.json').write_text(json.dumps(summary,indent=2))
    print(out, flush=True)
    print(json.dumps(outcome), flush=True)

if __name__ == '__main__':
    main()
