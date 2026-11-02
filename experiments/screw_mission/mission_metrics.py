#!/usr/bin/env python3
"""Mission accounting from independently matched detections, including undelivered ones.
Times MUST share one verified clock; clock error bounds are mandatory.
A matched_event_id is ground-truth association, never a model's own claimed ID.
"""
import argparse
import json
from pathlib import Path


def evaluate(gt, detections, clock_error_ms):
    if clock_error_ms < 0: raise ValueError('nonnegative clock uncertainty required')
    ids = [e['event_id'] for e in gt]
    if not ids or len(ids) != len(set(ids)): raise ValueError('nonempty unique GT required')
    events = {e['event_id']: e for e in gt}
    matched = {key: [] for key in ids}
    seen = set(); false_positive = 0
    for d in detections:
        if d['detection_id'] in seen: raise ValueError('duplicate detection_id')
        seen.add(d['detection_id'])
        key = d.get('matched_event_id')
        if key is None:
            false_positive += 1;continue
        if key not in events: raise ValueError('unknown ground-truth event')
        e = events[key]
        if not e['start_ms'] <= d['source_ms'] <= e['end_ms']:
            raise ValueError('matched detection source outside event visibility')
        matched[key].append(d)
    rows=[]
    for key,e in events.items():
        deadline=e['start_ms']+e['deadline_ms']
        if e['end_ms'] < e['start_ms'] or e['deadline_ms'] <= 0: raise ValueError('invalid GT interval')
        candidates=matched[key]
        state='model_miss' if not candidates else 'delivery_miss'
        best=None; uncertain=False
        for d in candidates:
            event_rx=d.get('event_receive_ms')
            evidence_rx=d.get('evidence_receive_ms')
            if event_rx is None or evidence_rx is None or not d.get('evidence_valid',False):continue
            if d.get('evidence_source_id') != d['source_id']:continue
            arrival=max(event_rx,evidence_rx)
            if arrival+clock_error_ms < d['source_ms']:raise ValueError('receipt before source')
            if best is None or arrival < best:best=arrival
            if arrival-clock_error_ms <= deadline < arrival+clock_error_ms:uncertain=True
        if best is not None:
            if best+clock_error_ms <= deadline:state='success'
            elif uncertain:state='clock_uncertain'
            else:state='late'
        rows.append(dict(event_id=key,status=state,deadline_ms=deadline,
                         completion_ms=best,matched_detections=len(candidates)))
    counts={s:sum(r['status']==s for r in rows)
            for s in ['success','model_miss','delivery_miss','late','clock_uncertain']}
    detected_events=sum(bool(v) for v in matched.values())
    duplicates=sum(max(0,len(v)-1) for v in matched.values())
    return dict(schema=1,gt_events=len(gt),**counts,
        mission_success_lower=counts['success']/len(gt),
        mission_success_upper=(counts['success']+counts['clock_uncertain'])/len(gt),
        detected_events=detected_events,event_recall=detected_events/len(gt),
        unmatched_detections=false_positive,repeat_observations=duplicates,
        clock_error_ms=clock_error_ms,events=rows,
        note='Event recall, not per-frame F1. Undelivered predictions must be included from source ledger.')


def main():
    p=argparse.ArgumentParser()
    p.add_argument('gt',type=Path);p.add_argument('detections',type=Path)
    p.add_argument('--clock-error-ms',type=float,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    load=lambda path:[json.loads(s) for s in path.read_text().splitlines() if s.strip()]
    report=evaluate(load(a.gt),load(a.detections),a.clock_error_ms)
    a.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='events'},indent=2))


if __name__=='__main__':main()
