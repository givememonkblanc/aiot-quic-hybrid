import json,statistics,sys
from pathlib import Path
p=Path(sys.argv[1]);meta=json.loads((p/'outcome.json').read_text());rows=[json.loads(x) for x in (p/'records.jsonl').read_text().splitlines()]
def stat(v):
 if not v:return None
 v=sorted(v)
 def q(f):
  x=(len(v)-1)*f;i=int(x);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(x-i)
 return dict(n=len(v),mean=statistics.mean(v),p50=q(.5),p95=q(.95),max=max(v))
result=[]
for stage in meta['stages']:
 if 'end' not in stage:continue
 lo=stage['start']+meta['settle_seconds'];hi=stage['end']
 def records(tag):return [r for r in rows if r['stage']==stage['index'] and r['tag']==tag and lo<=r['host']<=hi]
 def get(tag):return [r['data'] for r in records(tag)]
 pp=get('PROFILE');cap=[c for c in get('CAPTURE_PROFILE') if c['valid']];ai=get('AI_PROFILE');ac=get('AI_CPU');fr=get('FRAGMENT_PROFILE')
 s=dict(name=stage['name'],status=stage.get('status'),replay=stage['replay'],target=stage['fps_target'])
 if len(pp)>1:
  dt=pp[-1]['uptime_us']-pp[0]['uptime_us'];s.update(capture_or_replay_hz=(pp[-1]['frames']-pp[0]['frames'])*1e6/dt,cpu_pct=[100*(1-((pp[-1][f'idle{c}_us']-pp[0][f'idle{c}_us'])%2**32)/dt) for c in [0,1]],min_internal=min(x['internal_free'] for x in pp),min_psram=min(x['psram_free'] for x in pp),queue_peak=max(x['queue_bytes'] for x in pp))
 s.update(ai_wall_ms=stat([a['duration_us']/1000 for a in ai]),ai_pipeline_cpu_ms=stat([a['cpu_us']/1000 for a in ac]),fragment_cpu_ms=stat([f['cpu_us']/1000 for f in fr]),fragment_wall_ms=stat([f['wall_us']/1000 for f in fr]))
 if cap:
  gaps=[(b['copy_done_us']-a['copy_done_us'])/1000 for a,b in zip(cap,cap[1:])]
  s['camera']=dict(n=len(cap),jpeg_bytes=stat([c['bytes'] for c in cap]),sensor_dma_jpeg_ms=stat([(c['ready_us']-c['start_us'])/1000 for c in cap if c['ready_us']>=c['start_us']]),
   fb_get_wall_ms=stat([(c['get_done_us']-c['get_start_us'])/1000 for c in cap]),fb_get_cpu_ms=stat([c['get_cpu_us']/1000 for c in cap]),
   jpeg_copy_wall_ms=stat([c['copy_wall_us']/1000 for c in cap]),jpeg_copy_cpu_ms=stat([c['copy_cpu_us']/1000 for c in cap]),collection_gap_ms=stat(gaps),gaps_over_100ms=sum(g>100 for g in gaps),
   dispatch_gap_ms=stat([(b['get_start_us']-a['copy_done_us'])/1000 for a,b in zip(cap,cap[1:])]),
   long_gap_fb_wait_dominant=sum(b['copy_done_us']-a['copy_done_us']>100000 and b['get_done_us']-b['get_start_us']>(b['copy_done_us']-a['copy_done_us'])/2 for a,b in zip(cap,cap[1:])),
   long_gap_dispatch_dominant=sum(b['copy_done_us']-a['copy_done_us']>100000 and b['get_start_us']-a['copy_done_us']>(b['copy_done_us']-a['copy_done_us'])/2 for a,b in zip(cap,cap[1:])),
   collection_hz=(len(cap)-1)*1e6/(cap[-1]['copy_done_us']-cap[0]['copy_done_us']) if len(cap)>1 else None,
   fresh_age_at_get_ms=stat([(c['get_done_us']-c['start_us'])/1000 for c in cap]))
 pipe={r['data']['us']:r for r in records('PIPE_PROFILE')};batch={r['data']['us']:r for r in records('BATCH_PERF')};ts=sorted(pipe.keys()&batch.keys())
 if len(ts)>1:
  a,b=ts[0],ts[-1];d={k:pipe[b]['data'][k]-pipe[a]['data'][k] for k in pipe[a]['data'] if k not in ['us','wifi_registered','wifi_max_us']}
  bd={k:batch[b]['data'][k]-batch[a]['data'][k] for k in batch[a]['data'] if k not in ['us','kib','max_group','max_hold_us','all_lock_max_hold_us']}
  n=bd['fragments'];factor=121/n/1000 if stage['replay'] and n else None
  exclusive=dict(aead=d['aead_cpu'],header_protection=d['hp_cpu'],packetization_other=d['packet_cpu']-d['aead_cpu']-d['hp_cpu'],udp_submit=d['udp_cpu'],batch_other=bd['cpu_us']-d['packet_cpu']-d['udp_cpu'])
  s['transport']=dict(counters=d,batch_counters=bd,window_s=(b-a)/1e6,cpu_us_exclusive=exclusive,
   cpu_ms_per_123179B_frame={k:v*factor for k,v in exclusive.items()} if factor else None,
   lock_wait_ms_per_frame=bd['wait_us']*factor if factor else None,
   socket_wall_ms_per_frame=d['socket_wall']*factor if factor else None,socket_cpu_ms_per_frame=d['socket_cpu']*factor if factor else None,
   wifi_match_fraction=d['wifi_matched']/bd['packets'] if bd['packets'] else None,
   wifi_submit_to_done_ms_matched_only=d['wifi_us']/d['wifi_matched']/1000 if d['wifi_matched'] else None,
   wifi_completion_failures_matched_only=d['wifi_failed'],
   caveat='Unmatched callbacks are not packet loss. Sparse matches do not characterize full Wi-Fi TX latency; callback interval includes lwIP/queue/airtime/retries.')
 result.append(s)
(p/'pipeline-summary.json').write_text(json.dumps(result,indent=2)+'\n')
for s in result:print(json.dumps(dict(name=s['name'],hz=s.get('capture_or_replay_hz'),cpu_ms_per_frame=s.get('transport',{}).get('cpu_ms_per_123179B_frame'),wifi_match_fraction=s.get('transport',{}).get('wifi_match_fraction'),camera=s.get('camera'))))
