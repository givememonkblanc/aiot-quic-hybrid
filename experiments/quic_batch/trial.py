"""Characterize all-on camera/ESPDet/ROI/QUIC and admission-gate effects."""
import argparse,json,time
from datetime import datetime,timezone
from pathlib import Path
import serial
BASE=Path(__file__).resolve().parent
ROOT=BASE.parents[1]
FEED=ROOT/'experiments/handoff/results/cross-ap-final-mode2-03/feed.jsonl'
TAGS=['PROFILE','TASK_PROFILE','AI_START','AI_OFFER','AI_PROFILE','AI_CPU','ESPDET_PHASE','IMG_PROFILE','ROI_PROFILE','IMG_GATE','REPLAY','DGRAM_PERF','BATCH_PERF']

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--smoke',action='store_true');args=parser.parse_args()
 out=BASE/'results'/datetime.now(timezone.utc).strftime('batch-%Y%m%dT%H%M%SZ');out.mkdir(mode=0o700)
 seconds=25 if args.smoke else 60
 meta=dict(status='running',stage_seconds=seconds,settle_seconds=5 if args.smoke else 10,ai_core=1,ai_priority=1,trace=json.loads((BASE/'trace.json').read_text()),stages=[])
 print(out,flush=True)
 ser=serial.Serial(timeout=.15);ser.port='/dev/ttyACM0';ser.baudrate=115200;ser.dtr=False;ser.rts=False;ser.open()
 seen={'espdet':0,'network':False,'failed_ai':0};successful_seqs=set()
 raw=(out/'serial.log').open('wb');data=(out/'records.jsonl').open('w');feed=FEED.open('rb');feed.seek(0,2);rx=(out/'feed.jsonl').open('wb')
 def cmd(s):ser.write((s+'\n').encode());ser.flush()
 def read(stage=-1):
  b=ser.readline();s=b.decode(errors='replace');now=time.monotonic()
  if b:raw.write(b);raw.flush()
  incoming=feed.read();rx.write(incoming)
  if b'"kind":"publish"' in incoming or b'"kind": "publish"' in incoming:seen['network']=True
  for tag in TAGS:
   marker='['+tag+'] '
   if marker in s:
    try:r=json.loads(s.split(marker,1)[1])
    except ValueError:break
    data.write(json.dumps(dict(stage=stage,host=now,tag=tag,data=r))+'\n');data.flush()
    if tag=='ESPDET_PHASE':seen['espdet']+=1;successful_seqs.add(r['seq'])
    if tag=='AI_PROFILE' and r['seq'] not in successful_seqs:seen['failed_ai']+=1
    return tag,r
  return s,None
 def drain(seconds):
  end=time.monotonic()+seconds
  while time.monotonic()<end:read()
 try:
  drain(15)
  for c in ['mode 2','policy 1','aiprio 1','video 30 6144','profile 1','probe 0','replay 0']:cmd(c);drain(.3)
  drain(15)
  # Warm the same 1024 JPEG/model before exposing it to network load.
  for c in ['probe 5','replay 1','profile 3']:cmd(c);drain(.3)
  drain(20)
  if not seen['network']:raise RuntimeError('No received MQTT publication')
  order=[1,4,8,16,0] if args.smoke else [1,8,0,4,16,16,4,0,8,1]
  prev=None
  for idx,kib in enumerate(order):
   name=f'{kib or "frame"}KiB-r{idx//5+1}';profile=5;hz=5;replay=1
   cmd('profile 1');drain(8)
   for c in [f'batch {kib}','replay 0','probe 5','replay 1','profile 5']:cmd(c);drain(.25)
   start=time.monotonic();last=start;entry=dict(index=idx,name=name,profile=profile,hz=hz,replay=replay,batch_kib=kib,start=start);meta['stages'].append(entry)
   valid_before=seen['espdet'];fail_before=seen['failed_ai']
   print(name,'started',flush=True)
   while time.monotonic()-start<seconds:
    tag,r=read(idx);now=time.monotonic()
    if tag=='BATCH_PERF' and r['kib']!=kib:raise RuntimeError('Batch setting did not take effect')
    if tag=='PROFILE':
     last=now
     if prev is not None and r['uptime_us']<prev:raise RuntimeError('Reboot during measurement')
     prev=r['uptime_us']
     if r['internal_free']<16384 or r['psram_free']<131072:raise RuntimeError('Heap guard')
    if seen['failed_ai']-fail_before>=3:raise RuntimeError('Repeated AI decode/inference failures invalidate concurrent workload')
    if now-last>20:raise RuntimeError('Profile missing >20s')
    if isinstance(tag,str) and 'Task watchdog got triggered' in tag:
     cmd('profile 1')
     entry.update(end=time.monotonic(),status='watchdog_stop',reason='CPU idle watchdog; preview stopped immediately')
     print(name,'watchdog_stop',flush=True)
     break
    if isinstance(tag,str) and any(x in tag for x in ['Guru Meditation','assert failed']):raise RuntimeError('Panic/assert')
   if entry.get('status')!='watchdog_stop':
    if seen['espdet']==valid_before:raise RuntimeError('No successful AI inference in stage')
    entry.update(end=time.monotonic(),status='completed')
   entry.update(ai_successes=seen['espdet']-valid_before,ai_failures=seen['failed_ai']-fail_before)
   print(name,'completed',flush=True)
   (out/'outcome.json').write_text(json.dumps(meta,indent=2)+'\n')
  meta['status']='completed'
 except Exception as e:
  meta.update(status='stopped',reason=str(e))
  if meta['stages']:meta['stages'][-1].update(end=time.monotonic(),status='stopped')
 finally:
  # Keep OTA disabled until host restores the original full flash image.
  for c in ['profile 1','replay 0','probe 1']:cmd(c);drain(.2)
  drain(5);ser.close();raw.close();data.close();feed.close();rx.close();(out/'outcome.json').write_text(json.dumps(meta,indent=2)+'\n')
 print(out,flush=True);print(json.dumps(meta),flush=True)
if __name__=='__main__':main()
