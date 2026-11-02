import collections,hashlib,io,json,statistics,sys
from pathlib import Path
from PIL import Image
BASE=Path(__file__).resolve().parent
p=Path(sys.argv[1]);meta=json.loads((p/'outcome.json').read_text());rows=[json.loads(x) for x in (p/'records.jsonl').read_text().splitlines()]
complete=[];metadata=[];ram=[];chunks=[];key=None;buf=bytearray();seq=0;seen=set()
roi_ids={r['data']['gen_ms'] for r in rows if r['tag']=='ROI_PROFILE' and r['data']['accepted']}
for line in (p/'feed.jsonl').open(encoding='latin1'):
 try:r=json.loads(line)
 except ValueError:continue
 if r.get('kind')=='publish':
  try:d=json.loads(r['payload'])
  except (ValueError,KeyError):continue
  if r.get('topic')=='aiot/profile/detection':metadata.append((r['t']/1e6,d))
  if d.get('kind')=='ram':ram.append((r['t']/1e6,d))
 if r.get('kind')!='datagram' or r.get('track')!=3:continue
 b=r['payload'].encode('latin1');chunks.append((r['t']/1e6,len(b)));k=(r['cid'],r['gen_ts_ms'])
 if b.startswith(b'\xff\xd8'):key=k;buf=bytearray();seq=r['seq']
 if k!=key or r['seq']!=seq:buf=bytearray();key=None;continue
 buf.extend(b);seq=(seq+1)&0xffffffff
 if len(buf)>262144:key=None;buf=bytearray();continue
 if buf.endswith(b'\xff\xd9'):
  if k not in seen:
   try:
    im=Image.open(io.BytesIO(buf));dims=im.size;im.verify()
   except Exception:key=None;continue
   is_roi=k[1] in roi_ids
   complete.append(dict(host=r['t']/1e6,gen=k[1],bytes=len(buf),roi=is_roi,dimensions=dims,sha256=hashlib.sha256(buf).hexdigest()));seen.add(k)
   if is_roi and not (p/'received_roi.jpg').exists():(p/'received_roi.jpg').write_bytes(buf)
  key=None;buf=bytearray()
def mean(v):return statistics.mean(v) if v else None
def quant(v,q):
 if not v:return None
 v=sorted(v);x=(len(v)-1)*q;i=int(x);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(x-i)
result=[]
for stage in meta['stages']:
 lo=stage['start']+meta['settle_seconds'];hi=stage['end'];dt=hi-lo
 if dt<=0:continue
 def get(tag):return [r['data'] for r in rows if r['stage']==stage['index'] and lo<=r['host']<hi and r['tag']==tag]
 pp=get('PROFILE');ph=get('ESPDET_PHASE');ss={x['seq'] for x in ph};aa=[x for x in get('AI_PROFILE') if x['seq'] in ss];image=get('IMG_PROFILE');roi=get('ROI_PROFILE');rr=[x for x in complete if lo<=x['host']<hi]
 cpu=[]
 for c in (0,1):
  diffs=[]
  for a,b in zip(pp,pp[1:]):
   elapsed=b['uptime_us']-a['uptime_us'];idle=(b[f'idle{c}_us']-a[f'idle{c}_us'])%2**32
   if elapsed>0 and idle<=elapsed*1.02:diffs.append((elapsed,idle))
  cpu.append(100*(1-sum(x[1] for x in diffs)/sum(x[0] for x in diffs)) if diffs else None)
 task={};tt=get('TASK_PROFILE')
 for tid in {t['id'] for t in tt}:
  t=[x for x in tt if x['id']==tid];a,b=t[0],t[-1];elapsed=b['uptime_us']-a['uptime_us']
  if elapsed>0:task[a['name']]=dict(core=a['core'],pct=100*((b['runtime_us']-a['runtime_us'])%2**32)/elapsed,priority=a['priority'])
 accepted={x['ts_ms'] for x in image if x['accepted']}|{x['gen_ms'] for x in roi if x['accepted']};delivered={x['gen'] for x in complete if x['host']<=hi+5}
 received_ram=[r for t,r in ram if lo<=t<hi];counter={}
 if len(received_ram)>=2:
  for name in ['tx_failed','tx_expired']:counter[name]=received_ram[-1][name]-received_ram[0][name]
  counter['rtt_ms_mean']=mean([x['quic_rtt_ms'] for x in received_ram])
 s=dict(image_chunk_rx_KiBs=sum(n for t,n in chunks if lo<=t<hi)/dt/1024,stage=stage['index'],name=stage['name'],status=stage.get('status'),profile=stage['profile'],nominal_preview_hz=stage['hz'],replay=bool(stage['replay']),window_s=dt,ai_n=len(aa),ai_fps=len(aa)/dt,ai_ms_mean=mean([x['duration_us']/1000 for x in aa]),ai_ms_p95=quant([x['duration_us']/1000 for x in aa],.95),decode_ms=mean([x['decode_us']/1000 for x in ph]),detect_ms=mean([x['detect_us']/1000 for x in ph]),roi_ms=mean([x['roi_us']/1000 for x in ph]),image_attempts=len(image),image_admitted=sum(x['accepted'] for x in image),roi_attempts=len(roi),roi_admitted=sum(x['accepted'] for x in roi),attempt_KiBs=sum(x['bytes'] for x in image+roi)/dt/1024,admitted_KiBs=sum(x['bytes'] for x in image+roi if x['accepted'])/dt/1024,complete_KiBs=sum(x['bytes'] for x in rr)/dt/1024,complete_preview=sum(not x['roi'] for x in rr),complete_roi=sum(x['roi'] for x in rr),cohort_admitted=len(accepted),cohort_completed=len(accepted&delivered),gates=dict(collections.Counter(x['reason'] for x in get('IMG_GATE'))),cpu_pct=cpu,tasks=task,queue_peak=max((x['queue_bytes'] for x in pp),default=0),psram_peak=max((x['psram_used'] for x in pp),default=0),min_internal_free=min((x['internal_free'] for x in pp),default=0),input_fps=(pp[-1]['frames']-pp[0]['frames'])*1e6/(pp[-1]['uptime_us']-pp[0]['uptime_us']) if len(pp)>1 else None,replay_missed=sum(x['missed'] for x in get('REPLAY')),mqtt_received=len({x['source_seq'] for t,x in metadata if lo<=t<hi}),network=counter)
 result.append(s)
(p/'summary.json').write_text(json.dumps(result,indent=2)+'\n');(p/'complete_images.json').write_text(json.dumps(complete,indent=2)+'\n')
lines=['# ESPDet + camera + ROI + QUIC integration','',f'Status: {meta["status"]}. 45s stages; first10s excluded. Core1/priority1 AI, TX core1/priority5. Camera active throughout; replay stages feed a fixed cat JPEG to AI/TX. No server VLM in this experiment.','', '| Stage | AI mean / p95 ms | Attempt / admitted / complete KiB/s | Complete preview / ROI | Queue peak KiB | CPU0 / CPU1 % |','|---|---|---|---|---|---|']
def f(x):return '—' if x is None else f'{x:.1f}'
for s in result:lines.append(f'| {s["name"]} | {f(s["ai_ms_mean"])}/{f(s["ai_ms_p95"])} | '+ '/'.join(f(s[k]) for k in ['attempt_KiBs','admitted_KiBs','complete_KiBs'])+f' | {s["complete_preview"]}/{s["complete_roi"]} | {s["queue_peak"]/1024:.1f} | '+ '/'.join(f(x) for x in s['cpu_pct'])+' |')
lines+=['','## Admission reasons','', '| Stage | interval | admission | memory budget | queue slots | partial enqueue |','|---|---|---|---|---|---|']
for s in result:lines.append('| '+s['name']+' | '+' | '.join(str(s['gates'].get(k,0)) for k in ['interval','admission','memory_budget','queue_slots','enqueue_partial'])+' |')
lines+=['','## Scope','', '- Profile8 bypasses only video_admission_allow (including the nonempty-queue rejection). Minimum frame interval, queue capacity, heap/budget checks and 2s datagram expiry remain. No OOM is intentionally induced.', '- Complete goodput requires contiguous image chunks plus valid decoded JPEG structure. Admitted bytes are whole-image enqueue successes; a partial enqueue can still consume transport CPU but is excluded from admitted goodput.', '- ROI is the highest-score detected object only. It is generated on the MCU; fixture is a cat model smoke test, not screw accuracy. Generation timestamp differs from source capture timestamp and ROI_PROFILE records the mapping.', '- Replay FPS is supplied fixture FPS, not physical capture FPS. The final live stage uses current camera frames and may have no cats, hence no ROI workload.', '- CPU0/1 use idle-counter deltas; net_tx includes current QUIC image send path. CPU cost is implementation-specific, not evidence that QUIC inherently costs more than TCP.', '- Sequential short characterization with no repeats or network shaping. Rare tails, detection accuracy, server VLM correctness and power are not measured.', '- Baseline and restore evidence are stored separately with the full-flash restoration logs.']
(p/'REPORT.md').write_text('\n'.join(lines)+'\n');print('\n'.join(lines[:8+len(result)]))
