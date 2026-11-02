"""Live FOMO controlled weak-link impairment, no flash/reset/replay."""
import os,select,tty,time,json,re,signal
from weak_shaper import FlowShaper
from pathlib import Path
from datetime import datetime,timezone
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
FEED=ROOT/'experiments/handoff/results/cross-ap-final-mode2-03/feed.jsonl'
TAGS=['PROFILE','TASK_PROFILE','AI_START','AI_PHASE','AI_CPU','AI_PROFILE','IMG_PROFILE','ROI_PROFILE','DGRAM_PERF']
out=BASE/'results'/datetime.now(timezone.utc).strftime('weak-%Y%m%dT%H%M%SZ');out.mkdir(mode=0o700,parents=True);print(out,flush=True)
initial=json.loads((ROOT/'experiments/memory/results/latest.json').read_text())
meta=dict(status='running',initial=initial,seconds=45,settle_seconds=5,stages=[],input='live camera; no replay',firmware='restored FOMO baseline; no flash/reset')
fd=os.open('/dev/ttyACM0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK);tty.setraw(fd)
raw=(out/'serial.log').open('wb');records=(out/'records.jsonl').open('w');feed=FEED.open('rb');feed.seek(0,2);rx=(out/'feed.jsonl').open('wb');buf=b'';pending=[];acks=[];previous=0
last_profile=time.monotonic()
shaper=FlowShaper()
def read(stage=-1):
 global buf,previous,last_profile
 incoming=feed.read();rx.write(incoming)
 if not pending:
  if not select.select([fd],[],[],.1)[0]:return None,None
  b=os.read(fd,65536);raw.write(b);raw.flush();buf+=b
  while b'\n' in buf:
   line,buf=buf.split(b'\n',1);pending.append(line.decode(errors='replace'))
 if not pending:return None,None
 s=pending.pop(0);now=time.monotonic()
 match=re.search(r'event=camera_frame frames=(\d+) bytes=(\d+) ai_us=(\d+)',s)
 if match:
  records.write(json.dumps(dict(host=now,stage=stage,tag='CAMERA_EVENT',data=dict(frames=int(match[1]),bytes=int(match[2]),ai_us=int(match[3]))))+'\n');records.flush()
 if 'event=state' in s or 'event=link_' in s:
  records.write(json.dumps(dict(host=now,stage=stage,tag='LINK_EVENT',data=dict(line=s.strip())))+'\n');records.flush()

 if '[OK]' in s or '[ERR]' in s:acks.append(s.strip())
 if any(t in s for t in ['Task watchdog got triggered','Guru Meditation','assert failed']):raise RuntimeError('Watchdog/panic')
 for tag in TAGS:
  marker='['+tag+'] '
  if marker not in s:continue
  try:d=json.loads(s.split(marker,1)[1])
  except ValueError:return None,None
  records.write(json.dumps(dict(host=now,stage=stage,tag=tag,data=d))+'\n');records.flush()
  if tag=='PROFILE':
   last_profile=now
   if d['uptime_us']<previous:raise RuntimeError('Reboot')
   previous=d['uptime_us']
   if d['internal_free']<16384 or d['psram_free']<131072:raise RuntimeError('Heap guard')
  return tag,d
 return None,None
def drain(seconds):
 end=time.monotonic()+seconds
 while time.monotonic()<end:read()
def cmd(s,ack):
 before=len(acks);os.write(fd,(s+'\n').encode());end=time.monotonic()+3
 while time.monotonic()<end:
  read()
  if any(ack in a for a in acks[before:]):return
 raise RuntimeError('Command not acknowledged: '+s)
def stop(signum,frame):raise RuntimeError('Interrupted')
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
try:
 drain(2)
 for s,a in [('profile 1','profile=1'),('probe 0','probe_hz=0'),('video 30 6144','target_fps=30')]:cmd(s,a)
 stages=[('latest-good',5,1,False,30),('latest-weak',5,1,True,50),('latest-recovery',5,1,False,25),('quality-good',5,3,False,30),('quality-weak',5,3,True,50),('quality-recovery',5,3,False,25),('legacy-quality-good',0,3,False,30),('legacy-quality-weak',0,3,True,50),('legacy-quality-recovery',0,3,False,25)]
 for idx,(name,profile,policy,weak,duration) in enumerate(stages):
  if weak:shaper.on()
  elif shaper.created:shaper.off()
  for s,a in [(f'policy {policy}',f'video.policy={policy}'),(f'profile {profile}',f'profile={profile}')]:cmd(s,a)
  start=time.monotonic();stage=dict(index=idx,name=name,profile=profile,policy=policy,fps_target=30,probe_hz=0,start=start,weak=weak,duration_s=duration);meta['stages'].append(stage);last_profile=start
  print(name,'started',flush=True)
  while time.monotonic()-start<duration:
   read(idx)
   if profile and time.monotonic()-last_profile>15:raise RuntimeError('Profile missing')
  if weak:stage['shaper_stats']=shaper.stats()
  stage.update(end=time.monotonic(),status='completed');(out/'outcome.json').write_text(json.dumps(meta,indent=2)+'\n');print(name,'completed',flush=True)
 meta['status']='completed'
except Exception as e:
 meta.update(status='stopped',reason=str(e))
 if meta['stages']:meta['stages'][-1].update(end=time.monotonic(),status='stopped')
finally:
 shaper.off()
 meta['restore_acks']=[]
 try:
  for s,a in [('probe 0','probe_hz=0'),('profile 0','profile=0'),(f"mode {initial['mode']}",f"mobility.mode={initial['mode']}"),(f"policy {initial['video_policy']}",f"video.policy={initial['video_policy']}"),(f"video {initial['target_fps']} {initial['queue_budget']//1024}",f"target_fps={initial['target_fps']}")]:
   cmd(s,a);meta['restore_acks'].append(a)
  drain(3)
 except Exception as e:meta['restore_error']=str(e)
 os.close(fd);raw.close();records.close();rx.close();feed.close();meta['acks']=acks;meta['scope']='Receiver ingress impairment only: 256kbit/s, 200ms delay, 3% random loss, 128-packet limit. No RSSI or physical Wi-Fi retry simulation. FOMO model unchanged.';(out/'outcome.json').write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps(dict(status=meta['status'],reason=meta.get('reason'),restore=meta['restore_acks'],out=str(out))),flush=True)
