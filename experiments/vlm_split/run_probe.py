"""Deploy temporary ESPDet probe through existing OTA; retain baseline release."""
import hashlib,json,subprocess,sys,time
from datetime import datetime,timezone
from pathlib import Path
import serial
ROOT=Path(__file__).resolve().parents[2]
BASE=Path(__file__).resolve().parent
PROBE=BASE/'espdet_probe/build/aiot_quic.bin'
BASELINE=ROOT/'experiments/aiot_resource_management/results/sweep-core0-v26/previous-v25.bin'
SERVICE=ROOT/'experiments/ota/service.py'

def publish(p):
 r=subprocess.run([sys.executable,str(SERVICE),'publish',str(p)],capture_output=True,text=True,check=True)
 return json.loads(r.stdout)

def main():
 if not PROBE.exists() or not BASELINE.exists():raise RuntimeError('Missing probe/baseline artifact')
 if PROBE.stat().st_size>0x1f0000:raise RuntimeError('OTA slot exceeded')
 out=BASE/'results'/datetime.now(timezone.utc).strftime('probe-%Y%m%dT%H%M%SZ');out.mkdir()
 (out/'probe.bin').write_bytes(PROBE.read_bytes())
 meta={'status':'running','probe_sha256':hashlib.sha256(PROBE.read_bytes()).hexdigest(),'baseline_sha256':hashlib.sha256(BASELINE.read_bytes()).hexdigest(),'begin':False,'done':None,'baseline_core0_observed':False,'restore_acks':[]}
 raw=(out/'serial.log').open('wb');ser=None;deadline=time.monotonic()+180;published=False;restore_sent=False;restore_time=None
 try:
  while time.monotonic()<deadline:
   try:
    if ser is None:
     ser=serial.Serial(timeout=.25);ser.port='/dev/ttyACM0';ser.baudrate=115200;ser.dtr=False;ser.rts=False;ser.open()
     if not published:meta['publish']=publish(PROBE);published=True;print('probe published',out.name,flush=True)
    b=ser.readline()
    if not b:continue
    raw.write(b);raw.flush();s=b.decode(errors='replace')
    if '[ESPDET_BEGIN]' in s:
     meta['begin']=True;meta['release_restored']=publish(BASELINE);print('probe booted; release restored to baseline',flush=True)
    if '[ESPDET_DONE] ' in s:
     meta['done']=json.loads(s.split('[ESPDET_DONE] ',1)[1]);print(meta['done'],flush=True)
    if meta['begin'] and meta['done'] and '[AI_CPU] ' in s:
     r=json.loads(s.split('[AI_CPU] ',1)[1])
     if r['core']==0:meta['baseline_core0_observed']=True
    if meta['baseline_core0_observed'] and not restore_sent:
     for c in ['profile 0','probe 0','mode 2','policy 1','video 30 6144']:
      ser.write((c+'\n').encode());ser.flush();time.sleep(.2)
     restore_sent=True;restore_time=time.monotonic()
    if restore_sent and '[OK]' in s:meta['restore_acks'].append(s.strip())
    if restore_time and time.monotonic()-restore_time>10:
     meta['status']='completed' if meta['done']['status']=='completed' else 'probe_failed_restored';break
   except (serial.SerialException,OSError):
    if ser:
     try:ser.close()
     except Exception:pass
    ser=None;time.sleep(.5)
  else:meta['status']='timeout'
 except (Exception, KeyboardInterrupt) as e:meta.update(status='error',reason=str(e) or 'interrupted')
 finally:
  meta['release_restored']=publish(BASELINE)
  if ser:ser.close()
  raw.close();(out/'outcome.json').write_text(json.dumps(meta,indent=2)+'\n')
 print(out,flush=True);print(json.dumps(meta),flush=True)
if __name__=='__main__':main()
