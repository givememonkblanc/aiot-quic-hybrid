"""Direction tasks per run: a counted passage (serial [MOQ_PASSAGE]) is met when the helper result of the
frame sent for it ([MOQ_TASK] passage -> seq) reaches the laptop within T_max of the passage alert. Times
on the device capture clock: laptop arrival t minus the least camera lag (capture -> camera result), so
that 0 is the earliest the camera itself could alert on that frame."""
import json,re,sys
from pathlib import Path
T=float(sys.argv[1]); 
for run in map(Path,sys.argv[2:]):
    pas={};task={};lapsed=set();stats=None
    for l in open(run/'serial.log',errors='ignore'):
        m=re.match(r'\S+ \[(MOQ_PASSAGE|MOQ_TASK|MOQ_TASK_LAPSED|MOQ_TASKS)\] (\{.*\})',l)
        if not m: continue
        j=json.loads(m.group(2))
        if m.group(1)=='MOQ_PASSAGE': pas[j['id']]=j['seq']
        elif m.group(1)=='MOQ_TASK': task[j['passage']]=j['seq']
        elif m.group(1)=='MOQ_TASK_LAPSED': lapsed.add(j['passage'])
        else: stats=j
    cap={};lag=[];helper={}
    for l in open(run/'sub-remote'/'events.jsonl'):
        r=json.loads(l)
        if r.get('event') not in ('object','fetch_object'): continue
        if 'helper' in r:
            g=r['helper'].get('group')
            if g is not None and g not in helper: helper[g]=r['t']
        elif r.get('result') and '120' in r.get('properties',{}):
            c=int(r['properties']['120'])/1000; cap[r['result']['seq']]=c; lag.append(r['t']-c)
    off=min(lag)
    met=late=noresult=0; lat=[]
    for pid,seq in pas.items():
        if pid not in task or seq not in cap: continue
        g=task[pid]
        if g not in helper: noresult+=1; continue
        d=helper[g]-off-cap[seq]; lat.append(d)
        if d<=T: met+=1
        else: late+=1
    n=len(pas); lat.sort()
    p=lambda q: lat[min(len(lat)-1,int(q*len(lat)))] if lat else float('nan')
    print(f"{run.name:34s} passages {n:3d} sent {len(task):3d} lapsed {len(lapsed):3d} | met {met:3d} ({met/max(n,1):.0%}) late {late:3d} no-result {noresult:3d} | lat p50 {p(.5):.2f} p95 {p(.95):.2f} | dev {stats}")
