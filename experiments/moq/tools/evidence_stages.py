"""Stages of the evidence per run, on the laptop's serial clock: the source stored ([MOQ_EVIDENCE]),
the first planned conversion ([MOQ_MUS_PLAN] prepare), the representation ready ([MOQ_REPRESENTATION]),
its acknowledgement by the relay ([MOQ_RECEIPT] alias 3). Counts of sources evicted, sources never
represented, and the image streams that were in flight when each evidence object was published."""
import json,re,sys
from pathlib import Path
def q(v,x): v=sorted(v); return v[int(x*(len(v)-1))] if v else float('nan')
for run in map(Path,sys.argv[1:]):
    st={};plan={};rep={};ack={};evict=set();off_pub=[];off_done=[];slot=[]
    for l in open(run/'serial.log',errors='ignore'):
        m=re.match(r'(\S+) \[([A-Z_]+)\] (\{.*\})',l)
        if not m: continue
        t=float(m.group(1)); tag=m.group(2)
        try: j=json.loads(m.group(3))
        except Exception: continue
        if tag=='MOQ_EVIDENCE' and j.get('stored'): st.setdefault(j['group'],t)
        elif tag=='MOQ_MUS_PLAN' and j.get('do')=='prepare': plan.setdefault(j['group'],t)
        elif tag=='MOQ_REPRESENTATION': rep.setdefault(j['group'],t)
        elif tag=='MOQ_RECEIPT' and j.get('alias')==3: ack.setdefault(j['group'],t)
        elif tag=='MOQ_MUS_EVICT': evict.add(j['group'])
        elif tag=='MOQ_OFFLOAD' and j.get('result')==0: off_pub.append(t)
        elif tag=='MOQ_SLOT' and j.get('alias')==1: off_done.append((t,j['held_us']/1e6,j['bytes']))
    g=sorted(st)
    a=[plan[x]-st[x] for x in g if x in plan]; b=[rep[x]-plan[x] for x in g if x in plan and x in rep]
    c=[ack[x]-rep[x] for x in g if x in rep and x in ack]; tot=[ack[x]-st[x] for x in g if x in ack]
    # offload streams in flight when each evidence object was published
    busy=[sum(1 for (t,h,_) in off_done if t-h<=rep[x]<=t) for x in g if x in rep]
    held=[h for (_,h,_) in off_done]
    print(f"{run.name:30s} stored {len(g):3d} evicted {len(evict&set(g)):2d} planned {len(a):3d} represented {len(b):3d} acked {len(c):3d} | "
          f"store->plan {q(a,.5):.2f}/{q(a,.9):.2f} | codec {q(b,.5):.2f}/{q(b,.9):.2f} | rep->ack {q(c,.5):.2f}/{q(c,.9):.2f} | total {q(tot,.5):.2f}/{q(tot,.9):.2f} | "
          f"offload streams {len(off_done)} held {q(held,.5):.2f}/{q(held,.9):.2f} s, in flight at publish {sum(1 for x in busy if x)}/{len(busy)}")
