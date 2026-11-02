"""Characterize all-on camera/ESPDet/ROI/QUIC and admission-gate effects."""
import argparse,json,time
from datetime import datetime,timezone
from pathlib import Path
import serial
BASE=Path(__file__).resolve().parent
ROOT=BASE.parents[1]
FEED=ROOT/'experiments/handoff/results/cross-ap-final-mode2-03/feed.jsonl'
TAGS=['PROFILE','TASK_PROFILE','AI_START','AI_PROFILE','AI_CPU','ESPDET_PHASE','IMG_PROFILE','ROI_PROFILE','IMG_GATE','REPLAY']

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--ungated-only',action='store_true');args=parser.parse_args()
 out=BASE/'results'/datetime.now(timezone.utc).strftime('integrated-%Y%m%dT%H%M%SZ');out.mkdir()
 meta=dict(status='running',stage_seconds=45,settle_seconds=10,ai_core=1,ai_priority=1,stages=[])
 ser=serial.Serial(timeout=.15);ser.port='/dev/ttyACM0';ser.baudrate=115200;ser.dtr=False;ser.rts=False;ser.open()
 seen={'espdet':0}
 raw=(out/'serial.log').open('wb');data=(out/'records.jsonl').open('w');feed=FEED.open('rb');feed.seek(0,2);rx=(out/'feed.jsonl').open('wb')
 def cmd(s):ser.write((s+'\n').encode());ser.flush()
 def read(stage=-1):
  b=ser.readline();s=b.decode(errors='replace');now=time.monotonic()
  if b:raw.write(b);raw.flush()
  rx.write(feed.read())
  for tag in TAGS:
   marker='['+tag+'] '
   if marker in s:
    try:r=json.loads(s.split(marker,1)[1])
    except ValueError:break
    data.write(json.dumps(dict(stage=stage,host=now,tag=tag,data=r))+'\n');data.flush()
    if tag=='ESPDET_PHASE':seen['espdet']+=1
    return tag,r
  return s,None
 def drain(seconds):
  end=time.monotonic()+seconds
  while time.monotonic()<end:read()
 try:
  drain(15)
  for c in ['mode 2','policy 1','aiprio 1','video 30 6144','profile 3','probe 1','replay 1']:cmd(c);drain(.3)
  drain(15)
  if not seen['espdet']:raise RuntimeError('ESPDet inference not observed during warmup')
  stages=[('AI+metadata',3,1,1),('AI+ROI',4,1,1),('gated-1Hz',7,1,1),('gated-5Hz',7,5,1),('gated-10Hz',7,10,1),('ungated-1Hz',8,1,1),('ungated-5Hz',8,5,1),('ungated-10Hz',8,10,1),('live-all-10Hz',8,10,0)]
  if args.ungated_only:stages=[('ungated-1Hz',8,1,1),('ungated-5Hz',8,5,1),('ungated-10Hz',8,10,1),('live-all-10Hz',8,10,0)]
  prev=None
  for idx,(name,profile,hz,replay) in enumerate(stages):
   for c in [f'profile {profile}',f'probe {hz}',f'replay {replay}']:cmd(c)
   start=time.monotonic();last=start;entry=dict(index=idx,name=name,profile=profile,hz=hz,replay=replay,start=start);meta['stages'].append(entry)
   print(name,'started',flush=True)
   while time.monotonic()-start<45:
    tag,r=read(idx);now=time.monotonic()
    if tag=='PROFILE':
     last=now
     if prev is not None and r['uptime_us']<prev:raise RuntimeError('Reboot during measurement')
     prev=r['uptime_us']
     if r['internal_free']<16384 or r['psram_free']<131072:raise RuntimeError('Heap guard')
    if now-last>20:raise RuntimeError('Profile missing >20s')
    if isinstance(tag,str) and any(x in tag for x in ['Guru Meditation','Task watchdog got triggered','assert failed']):raise RuntimeError('Watchdog/panic')
   entry.update(end=time.monotonic(),status='completed')
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
