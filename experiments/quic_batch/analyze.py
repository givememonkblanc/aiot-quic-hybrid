"""Receiver-validated goodput and counter-delta analysis (no TX-success proxy)."""
import collections,hashlib,io,json,statistics,sys
from pathlib import Path
from PIL import Image
p=Path(sys.argv[1]);meta=json.loads((p/'outcome.json').read_text())
rows=[json.loads(x) for x in (p/'records.jsonl').read_text().splitlines()]
frames={};chunks=[];mqtt=[]
for line in (p/'feed.jsonl').open(encoding='latin1'):
 try:r=json.loads(line)
 except ValueError:continue
 if r.get('kind')=='publish' and r.get('topic')=='aiot/profile/detection':mqtt.append(r['t']/1e6)
 if r.get('kind')!='datagram' or r.get('track')!=3:continue
 b=r['payload'].encode('latin1');t=r['t']/1e6;k=(r['cid'],r['gen_ts_ms'])
 fr=frames.setdefault(k,dict(parts={},times={},gen=k[1]))
 if r['seq'] not in fr['parts']:
  fr['parts'][r['seq']]=b;fr['times'][r['seq']]=t;chunks.append((t,len(b)))
complete=[]
for f in frames.values():
 starts=[s for s,b in f['parts'].items() if b.startswith(b'\xff\xd8')]
 if len(starts)!=1:continue
 seq=starts[0];buf=bytearray();times=[]
 while seq in f['parts']:
  buf.extend(f['parts'][seq]);times.append(f['times'][seq]);seq=(seq+1)&0xffffffff
  if buf.endswith(b'\xff\xd9'):break
 if not buf.endswith(b'\xff\xd9'):continue
 try:im=Image.open(io.BytesIO(buf));dims=im.size;im.verify()
 except Exception:continue
 digest=hashlib.sha256(buf).hexdigest()
 complete.append(dict(host=max(times),gen=f['gen'],bytes=len(buf),dimensions=dims,sha256=digest,trace_match=digest==meta['trace']['sha256']))
def mean(v):return statistics.mean(v) if v else None
def quant(v,q):
 if not v:return None
 v=sorted(v);x=(len(v)-1)*q;i=int(x);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(x-i)
result=[]
phases={r['data']['seq']:r['data'] for r in rows if r['tag']=='ESPDET_PHASE'}
for stage in meta['stages']:
 start=stage['start']+meta['settle_seconds'];end=stage.get('end',start)
 perf=[r for r in rows if r['tag']=='BATCH_PERF' and r['stage']==stage['index'] and start<=r['host']<end]
 if len(perf)<2:continue
 a,b=perf[0],perf[-1];lo,hi=a['host'],b['host'];dt=hi-lo;cpu_dt=(b['data']['us']-a['data']['us'])/1e6
 delta={k:b['data'][k]-a['data'][k] for k in a['data'] if k not in ('us','kib','max_group','max_hold_us','all_lock_max_hold_us')}
 def get(tag):return [r['data'] for r in rows if r['tag']==tag and lo<r['host']<=hi]
 ai=get('AI_PROFILE');valid=[x for x in ai if x['seq'] in phases];lat=[x['duration_us']/1000 for x in valid]
 pp=get('PROFILE');tt=get('TASK_PROFILE');image=get('IMG_PROFILE');rr=[x for x in complete if lo<x['host']<=hi]
 admissions={x['ts_ms'] for x in image if x['accepted']};rxgen={x['gen'] for x in complete if x['trace_match'] and x['host']<=stage.get('end',hi)+8}
 task={}
 for name in {x['name'] for x in tt}:
  t=[x for x in tt if x['name']==name];u,v=t[0],t[-1];elapsed=v['uptime_us']-u['uptime_us']
  if elapsed>0:task[name]=100*((v['runtime_us']-u['runtime_us'])%2**32)/elapsed
 cpu=[]
 for c in (0,1):
  if len(pp)<2:cpu.append(None);continue
  elapsed=pp[-1]['uptime_us']-pp[0]['uptime_us'];idle=(pp[-1][f'idle{c}_us']-pp[0][f'idle{c}_us'])%2**32
  cpu.append(100*(1-idle/elapsed))
 goodbytes=sum(x['bytes'] for x in rr if x['trace_match'])
 s=dict(window_start_host=lo,window_end_host=hi,status=stage.get('status'),stage=stage['index'],name=stage['name'],batch_kib=stage['batch_kib'],window_s=dt,device_counter_window_s=cpu_dt,
  ai_success_n=len(valid),ai_attempt_n=len(ai),ai_failure_n=len(ai)-len(valid),ai_ms_mean=mean(lat),ai_ms_p50=quant(lat,.5),ai_ms_p95=quant(lat,.95),ai_ms_p99=quant(lat,.99),
  batch_tx_cpu_ms=delta['cpu_us']/1000,other_quic_io_cpu_ms=delta['other_quic_cpu_us']/1000,quic_io_cpu_pct=(delta['cpu_us']+delta['other_quic_cpu_us'])/cpu_dt/10000,
  tx_cpu_pct=delta['cpu_us']/cpu_dt/10000,task_cpu_pct=task,cpu_pct=cpu,
  completed_frames=sum(x['trace_match'] for x in rr),wrong_trace_frames=sum(not x['trace_match'] for x in rr),received_goodput_KiBs=goodbytes/dt/1024,
  received_fragment_KiBs=sum(n for t,n in chunks if lo<t<=hi)/dt/1024,
  image_attempts=len(image),image_admitted=len(admissions),admission_drops=sum(not x['accepted'] for x in image),admitted_not_completed=len(admissions-rxgen),
  cohort_expected_fragments=121*len(admissions),cohort_received_fragments=sum(len(f['parts']) for f in frames.values() if f['gen'] in admissions),
  replay_missed=sum(x['missed'] for x in get('REPLAY')),
  input_fps=(pp[-1]['frames']-pp[0]['frames'])*1e6/(pp[-1]['uptime_us']-pp[0]['uptime_us']) if len(pp)>1 else None,
  mqtt_results=sum(lo<t<=hi for t in mqtt),queue_peak=max((x['queue_bytes'] for x in pp),default=0),min_internal_free=min((x['internal_free'] for x in pp),default=0),
  counters=delta,mean_group_fragments=delta['fragments']/delta['calls'] if delta['calls'] else None,
  batch_lock_wait_us_mean=delta['wait_us']/delta['locks'] if delta['locks'] else None,
  locks_per_sent_frame=delta['locks']/(delta['fragments']/121) if delta['fragments'] else None,
  calls_per_sent_frame=delta['calls']/(delta['fragments']/121) if delta['fragments'] else None,
  batch_hold_max_cumulative_us=b['data']['max_hold_us'],
  quic_cpu_ms_per_received_KiB=(delta['cpu_us']+delta['other_quic_cpu_us'])/1000/(goodbytes/1024) if goodbytes else None)
 result.append(s)
(p/'summary.json').write_text(json.dumps(result,indent=2)+'\n');(p/'complete_images.json').write_text(json.dumps(complete,indent=2)+'\n')
for s in result:print(json.dumps({k:s[k] for k in ['name','ai_ms_mean','ai_failure_n','received_goodput_KiBs','tx_cpu_pct','quic_io_cpu_pct','calls_per_sent_frame','locks_per_sent_frame','batch_lock_wait_us_mean','admitted_not_completed']}))
