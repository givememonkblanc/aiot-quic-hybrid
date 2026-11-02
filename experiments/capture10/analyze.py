import json,statistics,sys,io
from pathlib import Path
from PIL import Image
p=Path(sys.argv[1]);m=json.loads((p/'outcome.json').read_text());rows=[json.loads(x) for x in (p/'records.jsonl').read_text().splitlines()]
def mean(v):return statistics.mean(v) if v else None
def quant(v,q):
 if not v:return None
 v=sorted(v);a=(len(v)-1)*q;i=int(a);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(a-i)
ram=[];frames={};complete=[]
for line in (p/'feed.jsonl').open(encoding='latin1'):
 try:r=json.loads(line)
 except ValueError:continue
 if r.get('kind')=='publish':
  try:d=json.loads(r['payload'])
  except (ValueError,KeyError):continue
  if d.get('kind')=='ram':ram.append((r['t']/1e6,d))
 if r.get('kind')!='datagram' or r.get('track')!=3:continue
 k=(r['cid'],r['gen_ts_ms']);f=frames.setdefault(k,{});f[r['seq']]=(r['payload'].encode('latin1'),r['t']/1e6)
for f in frames.values():
 starts=[i for i,(b,t) in f.items() if b.startswith(b'\xff\xd8')]
 if len(starts)!=1:continue
 seq=starts[0];b=bytearray();ts=[]
 while seq in f:
  chunk,t=f[seq];b.extend(chunk);ts.append(t);seq=(seq+1)%2**32
  if b.endswith(b'\xff\xd9'):break
 if not b.endswith(b'\xff\xd9'):continue
 try:im=Image.open(io.BytesIO(b));dims=im.size;im.verify()
 except Exception:continue
 complete.append(dict(host=max(ts),bytes=len(b),dimensions=dims))
result=[]
for stage in m['stages']:
 if 'end' not in stage:continue
 lo=stage['start']+m['settle_seconds'];hi=stage['end']
 def get(tag):return [r['data'] for r in rows if r['stage']==stage['index'] and lo<=r['host']<=hi and r['tag']==tag]
 pp=get('PROFILE');ai=get('AI_PROFILE');cp=get('AI_CPU');ph=get('AI_PHASE');tt=get('TASK_PROFILE')
 if len(pp)<2:continue
 dt=(pp[-1]['uptime_us']-pp[0]['uptime_us'])/1e6
 windows={}
 for span in [1,5,10]:
  v=[]
  for a,b in zip(pp,pp[span:]):
   elapsed=(b['uptime_us']-a['uptime_us'])/1e6
   if abs(elapsed-span)<.2:v.append((b['frames']-a['frames'])/elapsed)
  windows[str(span)]=dict(min=min(v) if v else None,p50=quant(v,.5),below_10=sum(x<9.999 for x in v),n=len(v))
 cpu=[100*(1-((pp[-1][f'idle{c}_us']-pp[0][f'idle{c}_us'])%2**32)/(dt*1e6)) for c in [0,1]]
 tasks={}
 for name in {t['name'] for t in tt}:
  t=[x for x in tt if x['name']==name];a,b=t[0],t[-1];elapsed=b['uptime_us']-a['uptime_us']
  if elapsed:tasks[name]=dict(pct=100*((b['runtime_us']-a['runtime_us'])%2**32)/elapsed,core=a['core'],priority=a['priority'],min_stack=min(x.get('stack_free',x.get('stack_hwm',0)) for x in t))
 received=[f for f in complete if lo<=f['host']<=hi];rr=[d for t,d in ram if lo<=t<=hi]
 drops={k:rr[-1][k]-rr[0][k] for k in ['tx_failed','tx_expired']} if len(rr)>1 else {}
 s=dict(name=stage['name'],status=stage.get('status'),target=stage['fps_target'],capture_hz=(pp[-1]['frames']-pp[0]['frames'])/dt,window_hz=windows,cpu_pct=cpu,tasks=tasks,
  ai_n=len(ai),ai_wall_ms=mean([x['duration_us']/1000 for x in ai]),ai_p95_ms=quant([x['duration_us']/1000 for x in ai],.95),ai_cpu_ms=mean([x['cpu_us']/1000 for x in cp]),decode_wall_ms=mean([x['decode_us']/1000 for x in ph]),model_wall_ms=mean([x['model_us']/1000 for x in ph]),
  internal_min=min(x['internal_free'] for x in pp),psram_min=min(x['psram_free'] for x in pp),internal_largest_min=min(x['internal_largest'] for x in pp),psram_largest_min=min(x['psram_largest'] for x in pp),queue_peak=max(x['queue_bytes'] for x in pp),drop_counters=drops,
  rx_fps=len(received)/(hi-lo),rx_KiBs=sum(x['bytes'] for x in received)/(hi-lo)/1024,dimensions=sorted({tuple(x['dimensions']) for x in received}))
 result.append(s)
(p/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
for s in result:print(json.dumps({k:s[k] for k in ['name','capture_hz','window_hz','cpu_pct','ai_wall_ms','ai_cpu_ms','queue_peak','rx_fps']}))
