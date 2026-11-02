"""Offline analysis: stage/phase rates, receiver recovery, and bounded memory."""
import json,sys,runpy,statistics
from pathlib import Path
p=Path(sys.argv[1]);scope=runpy.run_path(str(Path(__file__).parents[1]/'capture10/analyze.py'))
meta=json.loads((p/'outcome.json').read_text());rows=scope['rows'];complete=scope['complete'];ram=scope['ram']
pubs=[]
for line in (p/'feed.jsonl').open(encoding='latin1'):
 try:r=json.loads(line)
 except ValueError:continue
 if r.get('kind')!='publish':continue
 try:d=json.loads(r.get('payload',''))
 except (ValueError,TypeError):continue
 if 'capture_ms' in d and 'source_seq' in d:pubs.append(dict(host=r['t']/1e6,data=d))
def measure(idx,lo,hi):
 rr=[r for r in rows if r['stage']==idx and lo<=r['host']<hi]
 pp=[r['data'] for r in rr if r['tag']=='PROFILE'];ai=[r['data'] for r in rr if r['tag']=='AI_PROFILE'];cp=[r['data'] for r in rr if r['tag']=='AI_CPU'];roi=[r['data'] for r in rr if r['tag']=='ROI_PROFILE']
 r=dict(seconds=hi-lo,profile_samples=len(pp),ai_completions=len(ai),ai_positive=sum(a['detections']>0 for a in ai),metadata_received=sum(lo<=r['host']<hi for r in pubs),complete_jpegs=sum(lo<=r['host']<hi for r in complete),evidence_attempts=len(roi),evidence_accepted=sum(bool(a['accepted']) for a in roi))
 if ai:r['ai_wall_mean_ms']=statistics.mean(a['duration_us']/1000 for a in ai);r['ai_wall_max_ms']=max(a['duration_us']/1000 for a in ai)
 if cp:r['ai_cpu_mean_ms']=statistics.mean(a['cpu_us']/1000 for a in cp)
 if len(pp)>=2:
  dt=(pp[-1]['uptime_us']-pp[0]['uptime_us'])/1e6
  r.update(capture_hz=(pp[-1]['frames']-pp[0]['frames'])/dt,internal_min=min(a['internal_free'] for a in pp),psram_min=min(a['psram_free'] for a in pp),queue_peak=max(a['queue_bytes'] for a in pp),metadata_submit_delta=pp[-1]['metadata_ok']-pp[0]['metadata_ok'],evidence_submit_delta=pp[-1]['evidence_ok']-pp[0]['evidence_ok'])
  gaps=[(b['frames']-a['frames'])*1e6/(b['uptime_us']-a['uptime_us']) for a,b in zip(pp,pp[1:]) if b['uptime_us']>a['uptime_us']]
  r['capture_window_min_hz']=min(gaps) if gaps else None
 return r
out=[]
for s in meta['stages']:
 if 'end' not in s:continue
 idx=s['index'];lo=s['start']+5;hi=s['end'];d=dict(name=s['name'],all=measure(idx,lo,hi))
 if 'block_end' in s:
  a,b=s['block_start'],s['block_end'];d.update(outage_s=b-a,before=measure(idx,lo,a),outage=measure(idx,a,b),recovery10=measure(idx,b,min(b+10,hi)),recovered=measure(idx,min(b+10,hi),hi))
  for key,events in [('metadata',pubs),('jpeg',complete)]:
   after=[r['host'] for r in events if b<=r['host']<hi];before=[r['host'] for r in events if lo<=r['host']<a]
   d[key+'_first_after_unblock_s']=min(after)-b if after else None
   d[key+'_receiver_gap_s']=min(after)-max(before) if before and after else None
   d[key+'_received_during_block']=sum(a<=r['host']<b for r in events)
 out.append(d)
(p/'phase-summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
