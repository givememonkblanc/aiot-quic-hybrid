"""Exercise the real analyzer against reordered, duplicated, missing and corrupt fragments."""
import hashlib,json,subprocess,sys,tempfile
from pathlib import Path
base=Path(__file__).resolve().parent
jpeg=(base/'private/trace.jpg').read_bytes()
parts=[jpeg[i:i+1024] for i in range(0,len(jpeg),1024)]
assert len(parts)==121
keys=['calls','fragments','bytes','write_calls','packets','packet_bytes','locks','wait_us','hold_us','cpu_us','encode_us','udp_us','refused','budget_yields','failed','expired','dropped','short_groups','other_quic_cpu_us','all_lock_attempts','all_lock_acquired','all_lock_wait_us','all_lock_hold_us']
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)
 meta=dict(trace=dict(sha256=hashlib.sha256(jpeg).hexdigest()),settle_seconds=0,stages=[dict(index=0,name='test',start=9,end=21,batch_kib=1)])
 (p/'outcome.json').write_text(json.dumps(meta))
 rows=[]
 for host,value in [(10,0),(20,121)]:
  d={k:value for k in keys};d.update(us=int(host*1e6),kib=1,max_group=1,max_hold_us=1,all_lock_max_hold_us=1)
  rows.append(dict(stage=0,host=host,tag='BATCH_PERF',data=d))
 (p/'records.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
 feed=[]
 for gen in [1,2,3]:
  for idx in reversed(range(121)):
   if gen==2 and idx==50:continue
   b=parts[idx]
   if gen==3 and idx==70:b=bytes([b[0]^1])+b[1:]
   r=dict(kind='datagram',track=3,cid='fixture',gen_ts_ms=gen,seq=gen*1000+idx,payload=b.decode('latin1'),t=int((12+gen+idx/1000)*1e6))
   feed.append(r)
   if idx==10:feed.append(r.copy())
 (p/'feed.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in feed))
 subprocess.run([sys.executable,str(base/'analyze.py'),str(p)],check=True,stdout=subprocess.DEVNULL)
 s=json.loads((p/'summary.json').read_text())[0]
 assert s['completed_frames']==1 and s['wrong_trace_frames']==1,s
 assert abs(s['received_goodput_KiBs']-len(jpeg)/1024/10)<1e-9
 print('PASS: reordered valid JPEG counted once; duplicate, incomplete and wrong-hash JPEG excluded from goodput')
