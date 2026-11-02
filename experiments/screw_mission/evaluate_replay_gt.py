#!/usr/bin/env python3
"""Spatial scores for a reviewed static replay image, conditional on AI results.
Not unique passage success, not all-source-frame recall, not receiver QoS.
"""
import argparse
import json
from pathlib import Path
from evaluate_moq_count import parse_wire
from spatial_metrics import match_points


def main():
    p=argparse.ArgumentParser();p.add_argument('gt',type=Path);p.add_argument('suite',type=Path)
    p.add_argument('--max-distance',type=float,required=True,help='prespecified normalized centroid distance gate')
    p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    gt=json.loads(a.gt.read_text());suite=json.loads((a.suite/'manifest.json').read_text())
    high=next(r for r in suite['ladder']['representations'] if r['name']=='high')
    if not gt.get('reviewed') or gt['source_sha256']!=high['sha256']:
        p.error('reviewed GT must match the frozen original image hash')
    if len(gt['points'])!=gt['expected_count']:p.error('incomplete static labels')
    ids=[point['object_id'] for point in gt['points']]
    if len(set(ids))!=len(ids):p.error('duplicate ground-truth object identity')
    rows=[]
    for run in suite['results']:
        if run['status']!='complete':continue
        path=a.suite.parent/run['run']/'events.jsonl'
        for line in path.read_text().splitlines():
            e=json.loads(line)
            if e['event']!='object' or e.get('alias')!=2:continue
            detections=parse_wire(e['result']['result_hex'])['detections']
            predictions=[dict(class_id=d['class_id'],
                x=min(1,(d['x']+d['width']/2)/255),y=min(1,(d['y']+d['height']/2)/255)) for d in detections]
            rows.append(dict(run=run['run'],group=e['group'],
                **match_points(gt['points'],predictions,a.max_distance)))
    report=dict(schema=1,scope='conditional spatial F1 of received inference results on one static image',
        source_sha256=high['sha256'],gt_objects=len(gt['points']),distance_gate=a.max_distance,
        prediction_point='center of serialized normalized FOMO cell-component bounding region',rows=rows,
        note='Repeated frames are correlated. Unprocessed/undelivered frames and physical passage misses require the source/event ledger; they are not inferred here.')
    a.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(scored_results=len(rows),gt_objects=len(gt['points']))))


if __name__=='__main__':main()
