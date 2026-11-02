"""QoS observability from existing logs, without mixing MCU and host clocks."""
import json,statistics,sys
from pathlib import Path
p=Path(sys.argv[1]);summaries=json.loads((p/'summary.json').read_text());meta=json.loads((p/'outcome.json').read_text())
rows=[json.loads(x) for x in (p/'records.jsonl').read_text().splitlines()]
starts={r['data']['seq']:r['data'] for r in rows if r['tag']=='AI_START'}
valid={r['data']['seq'] for r in rows if r['tag']=='ESPDET_PHASE'}
complete=[f for f in json.loads((p/'complete_images.json').read_text()) if f['trace_match']]
rx=[]
for line in (p/'feed.jsonl').open(encoding='latin1'):
 try:r=json.loads(line)
 except ValueError:continue
 if r.get('kind')!='publish' or r.get('topic')!='aiot/profile/detection':continue
 try:d=json.loads(r['payload'])
 except ValueError:continue
 rx.append(dict(host=r['t']/1e6,source_seq=d['source_seq']))
def stats(v):
 if not v:return dict(n=0,mean=None,p50=None,p95=None,p99=None)
 v=sorted(v)
 def q(f):
  x=(len(v)-1)*f;i=int(x);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(x-i)
 return dict(n=len(v),mean=statistics.mean(v),p50=q(.5),p95=q(.95),p99=q(.99))
def gap(t,lo,hi):
 t=sorted(set(x for x in t if lo<x<=hi))
 return dict(received=len(t),interarrival_ms=stats([(b-a)*1000 for a,b in zip(t,t[1:])]),max_observed_silence_ms=max((b-a)*1000 for a,b in zip([lo]+t,t+[hi])),boundary_censored=True)
result=[]
for s in summaries:
 lo,hi=s['window_start_host'],s['window_end_host'];stage=next(t for t in meta['stages'] if t['index']==s['stage'])
 aa=[r['data'] for r in rows if r['tag']=='AI_PROFILE' and lo<r['host']<=hi and r['data']['seq'] in valid]
 start_age=[];ready_age=[]
 for a in aa:
  b=starts.get(a['seq'])
  if b is None:continue
  # Only MCU timestamps. The finish estimate adds the measured inference wall duration.
  age=(b['start_us']-b['capture_ms']*1000)/1000
  if age<0:continue
  start_age.append(age);ready_age.append(age+a['duration_us']/1000)
 completed_ids={a['seq'] for a in aa}
 received_ids={r['source_seq'] for r in rx if r['host']<=stage['end']+8}
 result.append(dict(name=s['name'],status=s['status'],batch_kib=s['batch_kib'],window_s=hi-lo,
  ai_start_age_ms=stats(start_age),ai_ready_age_estimate_ms=stats(ready_age),
  ai_ready_within_ms={str(d):sum(a<=d for a in ready_age) for d in [500,1000,2000,5000]},
  completed_ai_results=len(completed_ids),received_of_completed=len(completed_ids&received_ids),
  ai_result_service=gap([r['host'] for r in rx],lo,hi),preview_service=gap([r['host'] for r in complete],lo,hi),
  limitations=['AI-ready age uses one MCU clock; server one-way delay and AoI are not inferred.','AI-result cohort is not ground-truth event DESR; detection correctness and missed events are unknown.','Maximum observed silence includes censored measurement boundaries.']))
(p/'qos-profile.json').write_text(json.dumps(result,indent=2)+'\n')
for s in result:print(json.dumps(dict(name=s['name'],status=s['status'],ai_ready_p95_ms=s['ai_ready_age_estimate_ms']['p95'],ai_results=f"{s['received_of_completed']}/{s['completed_ai_results']}",preview_max_silence_ms=s['preview_service']['max_observed_silence_ms'])))
