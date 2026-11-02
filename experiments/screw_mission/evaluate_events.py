#!/usr/bin/env python3
"""Evaluate deadline-aware screw-pass mission success with a fixed GT denominator."""
import argparse
import json
from pathlib import Path


def read(path):
    return [json.loads(x) for x in path.read_text().splitlines() if x.strip()]


def main():
    p=argparse.ArgumentParser()
    p.add_argument("gt",type=Path,help="JSONL: event_id,start_ms,end_ms,deadline_ms")
    p.add_argument("detections",type=Path,
                   help="JSONL: source_ms,receive_ms,detected,evidence_valid")
    p.add_argument("--out",type=Path)
    a=p.parse_args();events=read(a.gt);detections=read(a.detections)
    used=set();rows=[]
    for event in events:
        candidates=[]
        for i,d in enumerate(detections):
            if i in used or not d.get("detected",False):continue
            if event["start_ms"]<=d["source_ms"]<=event["end_ms"]:
                candidates.append((d["receive_ms"],i,d))
        if not candidates:
            rows.append(dict(event_id=event["event_id"],status="missed"));continue
        _,i,d=min(candidates);used.add(i)
        deadline=event["start_ms"]+event["deadline_ms"]
        if not d.get("evidence_valid",False):status="invalid_evidence"
        elif d["receive_ms"]>deadline:status="late"
        else:status="success"
        rows.append(dict(event_id=event["event_id"],status=status,
                         age_ms=d["receive_ms"]-event["start_ms"],detection_index=i))
    counts={s:sum(r["status"]==s for r in rows)
            for s in ("success","missed","late","invalid_evidence")}
    report=dict(scope="deadline-aware physical screw-pass mission",gt_events=len(events),
                **counts,mission_success_rate=counts["success"]/max(len(events),1),
                unmatched_detections=len(detections)-len(used),events=rows)
    text=json.dumps(report,indent=2)
    if a.out:a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(text+"\n")
    print(text)


if __name__=="__main__":main()
