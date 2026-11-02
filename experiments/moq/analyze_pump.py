"""Summarize capture, successful service, and slot/pump timing without raw logs."""
import argparse,json
from pathlib import Path

def summarize(run):
 records=json.loads((run/'device-events.json').read_text())
 def tagged(tag):
  return [json.loads(r['line'].split(tag+' ',1)[1]) for r in records if tag+' {' in r['line']]
 def stats(values):
  values=sorted(values)
  if not values:return None
  return dict(count=len(values),mean=sum(values)/len(values),p50=values[int((len(values)-1)*.5)],p95=values[int((len(values)-1)*.95)],p99=values[int((len(values)-1)*.99)],max=values[-1])
 captures=tagged('[MOQ_CAPTURE]');pump=tagged('[MOQ_PUMP]');slots=tagged('[MOQ_SLOT]')
 events=[json.loads(s) for s in (run/'events.jsonl').read_text().splitlines()]
 images=[e for e in events if e['event']=='object' and e.get('alias',1)==1]
 ai=[e for e in events if e['event']=='object' and e.get('alias')==2]
 result=dict(run=run.name,outcome=json.loads((run/'outcome.json').read_text()),collected=len(captures),received_images=len(images),received_ai=len(ai),image_delivery_fraction=len(images)/len(captures),received_image_sizes=stats([e['bytes'] for e in images]),captured_image_sizes=stats([c['bytes'] for c in captures]),capture_acquire_us=stats([c['acquire_us'] for c in captures]),collection_gap_us=stats([c['gap_us'] for c in captures if c['gap_us']]),cache_misses=captures[-1]['cache_miss'],cache_overwrites=captures[-1]['overwritten'])
 if pump:
  result['pump']=dict(calls=sum(p['calls'] for p in pump),mean_us=sum(p['sum_us'] for p in pump)/sum(p['calls'] for p in pump),max_us=max(p['max_us'] for p in pump))
 for alias,name in [(1,'image'),(2,'ai')]:
  rows=[s for s in slots if s['alias']==alias]
  result[name+'_slot_us']={k:stats([s[k] for s in rows]) for k in ('held_us','submit_us','ack_wait_us')}
 return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('run',type=Path);args=p.parse_args();print(json.dumps(summarize(args.run),indent=2))
