"""Analyze fixed replay diagnostic; complete-image rather than chunk goodput."""
import hashlib,json,statistics as st,sys
from pathlib import Path

def quant(v,q):
    if not v:return None
    v=sorted(v);i=(len(v)-1)*q;a=int(i);return v[a]+(v[min(a+1,len(v)-1)]-v[a])*(i-a)

def analyze(p):
    meta=json.loads((p/'outcome.json').read_text());rows=[json.loads(x) for x in (p/'records.jsonl').read_text().splitlines()]
    complete=[];metadata=[];key=None;buf=bytearray();nxt=0;seen=set()
    for line in (p/'feed.jsonl').open(encoding='latin1'):
        try:r=json.loads(line)
        except ValueError:continue
        if r.get('kind')=='publish' and r.get('topic')=='aiot/profile/detection':
            try:metadata.append(dict(host=r['t']/1e6,data=json.loads(r['payload'])))
            except (ValueError,KeyError):pass
        if r.get('kind')!='datagram' or r.get('track')!=3:continue
        b=r.get('payload','').encode('latin1');k=(r.get('cid'),r.get('gen_ts_ms'));seq=r.get('seq')
        if b.startswith(b'\xff\xd8'):key=k;buf=bytearray();nxt=seq
        if key!=k or seq!=nxt:buf=bytearray();key=None;continue
        buf.extend(b);nxt+=1
        if len(buf)>262144:key=None;buf=bytearray();continue
        if buf.endswith(b'\xff\xd9'):
            if k not in seen and hashlib.sha256(buf).hexdigest()==meta['fixture']['sha256']:
                complete.append(dict(host=r['t']/1e6,bytes=len(buf),key=k));seen.add(k)
            key=None;buf=bytearray()
    result=[]
    for stage in meta['stages']:
        lo=stage['start']+meta['settle_seconds'];hi=stage['end'];dt=hi-lo
        if dt<=0:continue
        def get(tag):return [r['data'] for r in rows if r['stage']==stage['index'] and lo<=r['host']<hi and r['tag']==tag]
        profiles=get('PROFILE');aa=get('AI_PROFILE');starts={r['data']['seq']:r['data'] for r in rows if r['tag']=='AI_START'}
        aa=[r for r in aa if r['seq'] in starts and starts[r['seq']]['priority']==stage['priority']]
        durations=[r['duration_us']/1e6 for r in aa];ages=[(starts[r['seq']]['start_us']-starts[r['seq']]['capture_ms']*1000+r['duration_us'])/1e6 for r in aa]
        rx=[r for r in complete if lo<=r['host']<hi];images=get('IMG_PROFILE');replay=get('REPLAY');offers=get('AI_OFFER')
        cpu=[]
        for c in (0,1):
            good=[]
            for a,b in zip(profiles,profiles[1:]):
                elapsed=b['uptime_us']-a['uptime_us'];idle=(b[f'idle{c}_us']-a[f'idle{c}_us'])%2**32
                if elapsed>0 and 0<=idle<=elapsed*1.02:good.append((elapsed,idle))
            cpu.append(100*(1-sum(x[1] for x in good)/sum(x[0] for x in good)) if good else None)
        tasks={}
        tt=get('TASK_PROFILE')
        for tid in {r['id'] for r in tt}:
            t=[r for r in tt if r['id']==tid]
            if len(t)<2:continue
            a,b=t[0],t[-1];elapsed=b['uptime_us']-a['uptime_us']
            if elapsed>0:tasks[a['name']]=dict(core=a['core'],priorities=sorted({r['priority'] for r in t}),pct=100*((b['runtime_us']-a['runtime_us'])%2**32)/elapsed)
        goodput=sum(r['bytes'] for r in rx)/dt/1024
        s=dict(core=meta['core'],priority=stage['priority'],status=stage['status'],priority_verified=stage.get('priority_verified',False),window_s=dt,n=len(aa),ai_fps=len(aa)/dt,ai_mean_s=st.mean(durations) if durations else None,ai_p50_s=quant(durations,.5),ai_p95_s=quant(durations,.95),ai_p99_s=quant(durations,.99),source_age_p95_s=quant(ages,.95),cpu_pct=cpu,tasks=tasks,image_attempts=len(images),image_accepted=sum(r['accepted'] for r in images),complete_jpegs=len(rx),goodput_KiBs=goodput,replay_fps=len(replay)/dt,replay_missed=sum(r['missed'] for r in replay),ai_offers=len(offers),pending_overwrites=sum(r['overwrite'] for r in offers),queue_peak=max((r['queue_bytes'] for r in profiles),default=None),min_internal_free=min((r['internal_free'] for r in profiles),default=None),psram_peak=max((r['psram_used'] for r in profiles),default=None))
        accepted={r['ts_ms'] for r in images if r['accepted']}
        delivered={r['key'][1] for r in complete if r['host']<=hi+8}
        s['image_admission_rejected']=len(images)-sum(r['accepted'] for r in images)
        s['accepted_cohort_completed']=len(accepted & delivered)
        s['accepted_cohort_not_completed_within_8s_drain']=len(accepted-delivered)
        s['mqtt_received']=len({r['data']['source_seq'] for r in metadata if lo<=r['host']<hi})
        scheduled=get('AI_CPU');s['ai_scheduled_cpu_mean_s']=st.mean(r['cpu_us']/1e6 for r in scheduled) if scheduled else None
        s['cpu_core_seconds_per_complete_KiB']=sum(c/100 for c in cpu)/goodput if all(c is not None for c in cpu) and goodput>0 else None
        s['deadline_completion_fraction']={str(d):sum(t<=d for t in ages)/len(ages) if ages else None for d in (1,2,3,5)}
        s['grid']=[dict(deadline_s=d,min_goodput_KiBs=g,observed_all_completions_on_time=bool(ages) and max(ages)<=d,meets=bool(ages) and max(ages)<=d and goodput>=g) for d in (1,2,3,5) for g in (32,64,96)]
        result.append(s)
    (p/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    results=[]
    for arg in sys.argv[1:]:results.extend(analyze(Path(arg)))
    print('| Core | Prio | N | AI mean / p95 s | Source age p95 s | JPEG KiB/s | Complete/accepted | Replay FPS | CPU0/1 % |')
    print('|---|---|---|---|---|---|---|---|---|')
    def f(x):return '—' if x is None else f'{x:.2f}'
    for s in results:print(f'| {s["core"]} | {s["priority"]} | {s["n"]} | {f(s["ai_mean_s"])} / {f(s["ai_p95_s"])} | {f(s["source_age_p95_s"])} | {s["goodput_KiBs"]:.1f} | {s["complete_jpegs"]}/{s["image_accepted"]} | {s["replay_fps"]:.2f} | '+ '/'.join(f(x) for x in s['cpu_pct'])+' |')
