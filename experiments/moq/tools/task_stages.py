"""Stages of a direction task on the laptop clock (serial.log and helper.log carry the laptop's arrival
time): passage alert on the device -> frame published -> first byte at the helper -> frame complete ->
helper result. Medians and p95 per run."""
import json,re,sys
from pathlib import Path
def q(v,x): v=sorted(v); return v[int(x*(len(v)-1))] if v else float('nan')
for run in map(Path,sys.argv[1:]):
    pt={};task={};pub={}
    for l in open(run/'serial.log',errors='ignore'):
        m=re.match(r'(\S+) \[(MOQ_PASSAGE|MOQ_TASK|MOQ_OFFLOAD)\] (\{.*\})',l)
        if not m: continue
        t=float(m.group(1)); j=json.loads(m.group(3))
        if m.group(2)=='MOQ_PASSAGE': pt[j['id']]=t
        elif m.group(2)=='MOQ_TASK': task[j['passage']]=j['seq']
        else: pub[j['seq']]=t
    h={}
    for l in open(run/'helper.log',errors='ignore'):
        if '[HELPER_DET]' not in l: continue
        t=float(l.split(' ',1)[0]); j=json.loads(l.split(' ',2)[2])
        h.setdefault(j['group'],(t,j))
    st={k:[] for k in ('pub','queue','xfer','infer','total')}
    for p,g in task.items():
        if p not in pt or g not in pub: continue
        st['pub'].append(pub[g]-pt[p])
        if g in h:
            t,j=h[g]; end=t; recv=end-(j['end_us']-j['recv_us'])/1e6; first=recv-(j['recv_us']-j['first_us'])/1e6
            st['queue'].append(first-pub[g]); st['xfer'].append(recv-first); st['infer'].append((j['end_us']-j['start_us'])/1e6); st['total'].append(end-pt[p])
    print(f"{run.name:30s} n {len(st['total']):3d} | "+' | '.join(f"{k} {q(v,.5):.2f}/{q(v,.95):.2f}" for k,v in st.items())+f" | total<=2s {sum(x<=2 for x in st['total'])}")
