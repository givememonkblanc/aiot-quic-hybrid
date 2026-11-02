"""Analyze serial CPU/heap counters and actual MQTT receiver records."""
import csv
import json
import statistics as st
import sys
from pathlib import Path
p=Path(sys.argv[1])
meta=json.loads((p/'outcome.json').read_text())
rows=[json.loads(s) for s in (p/'samples.jsonl').read_text().splitlines()]
ais=[json.loads(s) for s in (p/'inferences.jsonl').read_text().splitlines()]
rx=[]
for r in csv.DictReader((p/'mqtt.csv').open()):
    if r['topic']=='aiot/profile/detection':
        try: rx.append((int(r['recv_us'])/1e6,json.loads(r['payload'])))
        except (ValueError,KeyError):pass
images=[r for r in csv.DictReader((p/'datagrams.csv').open()) if r['valid']=='1' and r['track_id']=='3']
summary=[]
for stage in meta['stages']:
    if 'end_monotonic' not in stage:continue
    idx=stage['index']; prof=stage['profile'];start=stage['start_monotonic']+meta['settle_seconds'];end=stage['end_monotonic']
    rr=[r for r in rows if r['stage']==idx and r['profile']==prof and r['phase_s']>=meta['settle_seconds']]
    aa=[r for r in ais if r['stage']==idx and r['phase_s']>=meta['settle_seconds']]
    if len(rr)<2:continue
    first,last=rr[0],rr[-1];dt=(last['uptime_us']-first['uptime_us'])/1e6
    cpu=[]
    for core in (0,1):
        good=[]
        for a,b in zip(rr,rr[1:]):
            delta=b['uptime_us']-a['uptime_us']
            idle=(b[f'idle{core}_us']-a[f'idle{core}_us'])%(2**32)
            if a['tasks'] and b['tasks'] and delta>0 and idle<=delta*1.02:
                good.append((delta,idle))
        cpu.append(100*(1-sum(x[1] for x in good)/sum(x[0] for x in good)) if good else None)
    received={(x['source_seq'],x['capture_ms']) for t,x in rx if start<=t<end and x['profile']==prof}
    img=[r for r in images if start<=int(r['recv_us'])/1e6<end]
    # Chunk bytes are not complete-frame goodput.
    summary.append(dict(stage=idx,profile=prof,samples=len(rr),capture_fps=(last['frames']-first['frames'])/dt,
        inference_fps=(last['ai_completed']-first['ai_completed'])/dt,
        ai_processing_ms_mean=st.mean(x['duration_us']/1000 for x in aa) if aa else None,
        ai_processing_ms_min=min((x['duration_us']/1000 for x in aa),default=None),
        ai_processing_ms_max=max((x['duration_us']/1000 for x in aa),default=None),
        ai_measurements=len(aa),positive_completions=sum(x['detections']>0 for x in aa),
        cpu0_busy_pct=cpu[0],cpu1_busy_pct=cpu[1],psram_peak=max(r['psram_used'] for r in rr),
        internal_peak=max(r['internal_used'] for r in rr),internal_min_free=min(r['internal_free'] for r in rr),
        internal_min_largest=min(r['internal_largest'] for r in rr),queue_peak=max(r['queue_bytes'] for r in rr),
        mqtt_received_unique=len(received),mqtt_receive_fps=len(received)/(end-start),
        metadata_enqueue_delta=last['metadata_ok']-first['metadata_ok'],evidence_enqueue_delta=last['evidence_ok']-first['evidence_ok'],
        image_chunks_received=len(img),image_chunk_bytes_received=sum(int(r['payload_len']) for r in img)))
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
names={1:'A Camera',2:'B +FOMO',3:'C +metadata',4:'D +evidence',5:'E +preview',6:'E2 queue discard'}
lines=['# A–E active-workload profiling','',f'Status: {meta["status"]}. 45s stages, first 15s excluded. A→E then E→A. Live scene; no ground truth. First A precedes lazy AI arena allocation; final A retains it.','',
'| Stage | Workload | Capture FPS | AI FPS | AI mean ms | CPU0 busy % | CPU1 busy % | PSRAM peak MiB | Queue peak KiB | MQTT received |',
'|---|---|---:|---:|---:|---:|---:|---:|---:|---:|']
for s in summary:
    ai='—' if s['ai_processing_ms_mean'] is None else f'{s["ai_processing_ms_mean"]:.0f}'
    c0='—' if s['cpu0_busy_pct'] is None else f'{s["cpu0_busy_pct"]:.1f}'
    c1='—' if s['cpu1_busy_pct'] is None else f'{s["cpu1_busy_pct"]:.1f}'
    lines.append(f'| {s["stage"]} | {names[s["profile"]]} | {s["capture_fps"]:.2f} | {s["inference_fps"]:.3f} | {ai} | {c0} | {c1} | {s["psram_peak"]/2**20:.2f} | {s["queue_peak"]/1024:.1f} | {s["mqtt_received_unique"]} |')
lines+=['','## Scope and controls','',
'- Same existing person FOMO weights, 96×96 model input and 1024×1024 camera configuration. Requested capture 30FPS; mode 2/policy 1 adaptive video admission; queue budget 6MiB. Camera-to-AI offer is independent of video admission in all profiles.',
'- A is camera-only ACTIVITY in a combined firmware. First A has zero completed inference and precedes lazy arena allocation; final A retains the arena after B–E. Report them separately. Decoder, task stacks, Wi-Fi/QUIC and evidence buffers are common reservations; neither A is the minimum memory of a standalone camera build. The actual compiled person model reserves 384KiB arena (not the older default vehicle asset’s 1MiB).',
'- Two extra 256KiB evidence/copy buffers are reserved across all stages (one at boot, one before profile sampling settles). Their 512KiB cost is included in all profiles; evidence copy/transmission occurs only in D. Absolute memory must not be compared directly to the previous firmware pilot.',
'- A/B send no detection metadata. C/D/E publish one MQTT QoS0 result per newly observed inference result, including zero-detection results; D also attempts one original-source JPEG per positive result. Positive results are not verified correct detections, nor unique physical object events.',
'- D uses the same best-effort image DATAGRAM path as preview, including admission and expiry. Enqueued evidence is not proof of complete JPEG reception. Chunk counts/bytes are not complete-image goodput.',
'- Wi-Fi/QUIC and RAM diagnostics (~5s) remain active for every profile. Synthetic camera status/event traffic is suppressed. OTA polling is suspended during profiles. Serial instrumentation is common to all stages.',
'- CPU utilization uses per-core IDLE task runtime differences / elapsed ESP timer time; it includes scheduler/interrupt effects and is not isolated CNN compute cost. AI duration wraps ai_inference_run, including preprocessing and scheduling delays.',
'- First 15s excluded to drain pending inference/queues. ABBA-like forward/reverse order does not make live inputs/network traces identical or create independent statistical repetitions.',
'- Heap peaks are 1Hz observed allocated-byte maxima, excluding static memory; internal/PSRAM reported separately. Task counters and metadata receive windows have different boundaries, so do not divide them to claim packet loss.',
'- No F1/mAP, energy, event-ground-truth DESR, handover or synchronized end-to-end latency is claimed.', '',f'Profile restore acknowledged: {meta.get("profile_restore_ack",False)}. Raw serial, per-inference durations, memory/CPU counters and receiver CSV are retained locally.']
(p/'REPORT.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines[:len(summary)+7]))
