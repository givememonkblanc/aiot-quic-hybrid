"""Aggregate equal-fixture repeats without implying statistical significance."""
import json,statistics,sys
from pathlib import Path
p=Path(sys.argv[1]);ss=json.loads((p/'summary.json').read_text());rows=[json.loads(x) for x in (p/'records.jsonl').read_text().splitlines()]
valid_seqs={r['data']['seq'] for r in rows if r['tag']=='ESPDET_PHASE'}
def quant(v,q):
 if not v:return None
 v=sorted(v);x=(len(v)-1)*q;i=int(x);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(x-i)
meta=json.loads((p/'outcome.json').read_text())
stability=[dict(batch_kib=k,attempted=sum(t['batch_kib']==k for t in meta['stages']),completed=sum(t['batch_kib']==k and t.get('status')=='completed' for t in meta['stages']),watchdog_stops=sum(t['batch_kib']==k and t.get('status')=='watchdog_stop' for t in meta['stages'])) for k in [1,4,8,16,0]]
(p/'stability.json').write_text(json.dumps(stability,indent=2)+'\n')
summary=[]
for kib in [1,4,8,16,0]:
 v=[s for s in ss if s['batch_kib']==kib and s['status']=='completed']
 if not v:continue
 wall=sum(s['window_s'] for s in v);device=sum(s['device_counter_window_s'] for s in v)
 counters={k:sum(s['counters'][k] for s in v) for k in v[0]['counters']}
 latency=[r['data']['duration_us']/1000 for r in rows if r['tag']=='AI_PROFILE' and r['data']['seq'] in valid_seqs and any(s['window_start_host']<r['host']<=s['window_end_host'] for s in v)]
 complete=sum(s['completed_frames'] for s in v);good_kib=complete*123179/1024
 summary.append(dict(batch_kib=kib,stability=next(t for t in stability if t['batch_kib']==kib),repeats=len(v),ai_n=len(latency),ai_failure_n=sum(s['ai_failure_n'] for s in v),ai_ms_mean=statistics.mean(latency) if latency else None,ai_ms_p50=quant(latency,.5),ai_ms_p95=quant(latency,.95),ai_ms_p99=quant(latency,.99),goodput_KiBs=good_kib/wall,goodput_repeat_range=[min(s['received_goodput_KiBs'] for s in v),max(s['received_goodput_KiBs'] for s in v)],completed_frames=complete,
  quic_io_cpu_pct=(counters['cpu_us']+counters['other_quic_cpu_us'])/device/10000,tx_cpu_pct=counters['cpu_us']/device/10000,quic_cpu_ms_per_received_KiB=(counters['cpu_us']+counters['other_quic_cpu_us'])/1000/good_kib if good_kib else None,
  submission_calls_per_sent_frame=counters['calls']*121/counters['fragments'],locks_per_sent_frame=counters['locks']*121/counters['fragments'],packetizer_calls_per_sent_frame=counters['write_calls']*121/counters['fragments'],udp_outputs_per_sent_frame=counters['packets']*121/counters['fragments'],mean_batch_lock_wait_us=counters['wait_us']/counters['locks'],total_batch_lock_wait_ms=counters['wait_us']/1000,total_all_lock_wait_ms=counters['all_lock_wait_us']/1000,
  batch_wait_ms_per_sent_frame=counters['wait_us']/1000*121/counters['fragments'],batch_cpu_ms_per_sent_frame=counters['cpu_us']/1000*121/counters['fragments'],all_locks_per_s=counters['all_lock_acquired']/device,
  offered_frames=sum(s['image_attempts'] for s in v),admission_drops=sum(s['admission_drops'] for s in v),admitted_frames=sum(s['image_admitted'] for s in v),admitted_not_completed=sum(s['admitted_not_completed'] for s in v),cohort_expected_fragments=sum(s['cohort_expected_fragments'] for s in v),cohort_received_fragments=sum(s['cohort_received_fragments'] for s in v),input_fps_range=[min(s['input_fps'] for s in v),max(s['input_fps'] for s in v)],queue_peak=max(s['queue_peak'] for s in v),min_internal_free=min(s['min_internal_free'] for s in v),counters=counters))
(p/'aggregate.json').write_text(json.dumps(summary,indent=2)+'\n')
for s in summary:print(json.dumps({k:v for k,v in s.items() if k!='counters'}))
