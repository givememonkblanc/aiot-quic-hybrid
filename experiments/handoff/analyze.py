#!/usr/bin/env python3
"""Analyze device-clock handoffs and receiver-clock delivery; no clock subtraction across hosts."""
import argparse
import json
import re
from pathlib import Path


def analyze(directory):
    directory = Path(directory)
    events = []
    for line in (directory/'serial.log').read_text(errors='replace').splitlines():
        if 'MOBILITY: event=' not in line:
            continue
        event = dict(re.findall(r'(\w+)=([^\s]+)', line.split('MOBILITY: ', 1)[1]))
        for key in ('t_us', 'seq', 'session', 'critical', 'queued', 'result', 'elapsed_ms', 'outage_ms', 'age_ms'):
            if key in event:
                try: event[key] = int(event[key])
                except ValueError: pass
        if event.get('event') == 'research_start':
            events.clear()  # Discard pre-reboot buffered lines.
        if event.get('event') in ('generate', 'link_down', 'link_up', 'session_ready', 'path_validation') and not isinstance(event.get('t_us'), int):
            continue  # Serial command output can truncate a log line.
        events.append(event)
    received = []
    feed = directory/'feed_snapshot.jsonl'
    if not feed.exists(): feed = directory/'feed.jsonl'
    for line in feed.read_text().splitlines() if feed.exists() else []:
        row = json.loads(line)
        try: payload = json.loads(row.get('payload', ''))
        except (ValueError, TypeError): continue
        if isinstance(payload, dict) and {'seq', 'gen_us', 'critical'} <= payload.keys():
            received.append({**payload, 'recv_us': row['t'], 'cid': row['cid']})
    # Both seq and generation timestamp are needed: seq restarts on reboot.
    identity = lambda e: (e['seq'], e.get('gen_us', e.get('t_us')))
    received_ids = {identity(r) for r in received}
    handoffs = []
    downs = [i for i,e in enumerate(events) if e['event'] == 'link_down']
    for n, i in enumerate(downs):
        down = events[i]
        following = events[i+1:downs[n+1] if n+1 < len(downs) else len(events)]
        up = next((e for e in following if e['event'] == 'link_up'), None)
        validation = next((e for e in following if e['event'] == 'path_validation' and e.get('result') == 2), None)
        prior_ready = next((e for e in reversed(events[:i]) if e['event'] == 'session_ready'), None)
        new_sessions = [e for e in following if e['event'] == 'session_begin']
        fallbacks = [e for e in following if e['event'] == 'fallback']
        # Use generation identities to locate packets before/after handoff on receiver clock.
        before = [r for r in received if down['t_us']-5000000 <= r['gen_us'] < down['t_us']]
        after = [r for r in received if r['gen_us'] >= down['t_us'] and
                 (n+1 == len(downs) or r['gen_us'] < events[downs[n+1]]['t_us'])]
        pre = max(before, key=lambda r: r['recv_us'], default=None)
        post = min((r for r in after if pre is None or r['recv_us'] > pre['recv_us']), key=lambda r:r['recv_us'], default=None)
        preserved = bool(prior_ready and validation and not new_sessions and not fallbacks and
                         validation['session'] == down['session'] == prior_ready['session'] and
                         pre and post and pre['cid'] == post['cid'])
        first_critical = next((e for e in following if e['event'] == 'generate' and e.get('critical') == 1), None)
        critical_enqueue = next((e for e in following if first_critical and e['event'] == 'enqueue' and
                                 e.get('seq') == first_critical['seq']), None)
        handoffs.append({'down_us': down['t_us'], 'session': down.get('session'),
            'first_outage_critical_enqueue_age_ms': critical_enqueue.get('age_ms') if critical_enqueue else None,
            'ready_before_handoff': bool(prior_ready and prior_ready['session'] == down.get('session')),
            'wifi_outage_ms': (up['t_us']-down['t_us'])/1000 if up else None,
            'path_validation_ms': validation.get('elapsed_ms') if validation else None,
            'session_preserved': preserved, 'new_sessions': len(new_sessions), 'fallbacks': len(fallbacks),
            'receiver_generation_boundary_gap_ms': (post['recv_us']-pre['recv_us'])/1000 if pre and post else None})
    # Steady trial window starts at first MQTT ready; exclude final 2 seconds for delivery drain.
    ready = next((e for e in events if e['event'] == 'session_ready'), None)
    end = max((e.get('t_us',0) for e in events), default=0)-2000000
    generated = [e for e in events if e['event'] == 'generate' and ready and ready['t_us'] <= e['t_us'] <= end]
    delivery = {}
    for name, critical in [('critical',1), ('telemetry',0)]:
        sent = {identity(e) for e in generated if e['critical'] == critical}
        delivered = len(sent & received_ids)
        delivery[name] = {'generated': len(sent), 'received': delivered,
                          'ratio': delivered/len(sent) if sent else None}
    return {'trial': directory.name, 'handoffs': handoffs, 'delivery_after_first_ready': delivery,
            'receiver_connections': sorted({r['cid'] for r in received}),
            'note': 'Pilot only. Receiver boundary gap includes generation cadence and buffering; not one-way delay. Delivery window excludes startup and final 2s.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trials', type=Path, nargs='+')
    args = parser.parse_args()
    for trial in args.trials:
        result = analyze(trial)
        (trial/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result, ensure_ascii=False))

if __name__ == '__main__': main()
