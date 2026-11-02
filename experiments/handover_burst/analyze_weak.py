"""Summarize controlled link impairment; RSSI itself is not manipulated."""
import json,sys,runpy,statistics,math
from pathlib import Path
p=Path(sys.argv[1]);scope=runpy.run_path(str(Path(__file__).parents[1]/'capture10/analyze.py'))
meta=json.loads((p/'outcome.json').read_text());rows=scope['rows'];complete=scope['complete'];ram=scope['ram'];q=scope['quant'];pubs=[];ai_datagrams=[];seen_ai=set()
for line in (p/'feed.jsonl').open(encoding='latin1'):
 try:r=json.loads(line)
 except ValueError:continue
 if r.get('kind')=='datagram' and r.get('track')==2:
  key=(r.get('cid'),r.get('seq'))
  if key not in seen_ai:ai_datagrams.append((r['t']/1e6,r));seen_ai.add(key)
 if r.get('kind')!='publish':continue
 try:d=json.loads(r.get('payload',''))
 except (ValueError,TypeError):continue
 if d.get('kind')=='ram':continue
 pubs.append((r['t']/1e6,d))
result=[]
for st in meta['stages']:
 if 'end' not in st:continue
 lo=st['start']+meta['settle_seconds'];hi=st['end'];duration=hi-lo
 def get(tag):return [r for r in rows if r['stage']==st['index'] and lo<=r['host']<hi and r['tag']==tag]
 cam=get('CAMERA_EVENT');pr=[r['data'] for r in get('PROFILE')];ai=[r['data'] for r in get('AI_PROFILE')];cpu=[r['data'] for r in get('AI_CPU')];rx=[r for r in complete if lo<=r['host']<hi];rr=[d for t,d in ram if lo<=t<hi];allpub=[d for t,d in pubs if lo<=t<hi]
 if st['profile']:
  det=[d for d in allpub if 'source_seq' in d and 'capture_ms' in d]
 else:det=[d for t,d in ai_datagrams if lo<=t<hi]
 mem=pr or rr
 d=dict(name=st['name'],status=st['status'],profile=st['profile'],policy=st['policy'],weak=st['weak'],analysis_seconds=duration,camera_events=len(cam),ai_completions=len(ai),ai_hz=len(ai)/duration,ai_wall_mean_ms=statistics.mean(a['duration_us']/1000 for a in ai) if ai else None,ai_wall_p95_ms=q([a['duration_us']/1000 for a in ai],.95),ai_cpu_mean_ms=statistics.mean(a['cpu_us']/1000 for a in cpu) if cpu else None,complete_rx_fps=len(rx)/duration,complete_rx_KiBs=sum(a['bytes'] for a in rx)/duration/1024,metadata_received=len(det),ai_delivery_path='MQTT profile metadata' if st['profile'] else 'QUIC DATAGRAM track2',all_non_ram_publications=len(allpub),memory_source='serial 1s PROFILE' if pr else 'received RAM telemetry (may be delayed)',memory_samples=len(mem))
 if cam:
  d['capture_hz_host']=((cam[-1]['data']['frames']-cam[0]['data']['frames'])/(cam[-1]['host']-cam[0]['host'])) if len(cam)>1 else None
  d['jpeg_mean_bytes']=statistics.mean(a['data']['bytes'] for a in cam)
  # Full 5s host-time bins; serial timestamps are observation times, not capture deadlines.
  bins=[sum(lo+i*5<=a['host']<lo+(i+1)*5 for a in cam)/5 for i in range(int(duration//5))]
  d['capture_5s_min_hz']=min(bins) if bins else None
 if len(pr)>1:
  dt=pr[-1]['uptime_us']-pr[0]['uptime_us'];d['capture_hz_device']=(pr[-1]['frames']-pr[0]['frames'])*1e6/dt
  d['cpu_pct']=[100*(1-((pr[-1][f'idle{c}_us']-pr[0][f'idle{c}_us'])%2**32)/dt) for c in [0,1]]
 if mem:d.update(internal_min=min(a['internal_free'] for a in mem),psram_min=min(a['psram_free'] for a in mem),queue_sample_peak=max(a['queue_bytes'] for a in mem))
 if not pr and len(rr)>1:
  dt=rr[-1]['uptime_us']-rr[0]['uptime_us']
  if dt>0:d['capture_hz_device']=(rr[-1]['frames']-rr[0]['frames'])*1e6/dt;d['capture_counter_source']='received RAM device timestamps; delayed delivery may shift phase boundaries'
 if rr:
  d['rtt_ms_median']=statistics.median(a['quic_rtt_ms'] for a in rr);d['rtt_ms_max']=max(a['quic_rtt_ms'] for a in rr)
  d['tx_failed_delta']=rr[-1]['tx_failed']-rr[0]['tx_failed'];d['tx_expired_delta']=rr[-1]['tx_expired']-rr[0]['tx_expired']
 d['state_events']=[r['data']['line'] for r in get('LINK_EVENT')]
 result.append(d)
(p/'weak-summary.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
