"""Verify baseline CLI ACKs and AI core after full-flash restore; never save NVS."""
import json,os,time
from pathlib import Path
import serial
base=Path(__file__).resolve().parent
out=base/'results/restored-serial.log'
ser=serial.Serial(timeout=.15);ser.port='/dev/ttyACM0';ser.baudrate=115200;ser.dtr=False;ser.rts=False;ser.open()
log=out.open('wb');os.chmod(out,0o600);acks=[];ai=[];boot=[]
def drain(seconds):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  b=ser.readline()
  if not b:continue
  log.write(b);s=b.decode(errors='replace')
  if '[OK]' in s:acks.append(s.strip())
  if '[AI_CPU] ' in s:
   try:ai.append(json.loads(s.split('[AI_CPU] ',1)[1]))
   except ValueError:pass
  if 'causal-core0-v25' in s or ('core=' in s and 'AI' in s):boot.append(s.strip())
try:
 drain(12)
 for c in ['profile 0','probe 0','mode 2','policy 1','video 30 6144']:
  ser.write((c+'\n').encode());ser.flush();drain(.6)
 drain(25)
finally:ser.close();log.close()
r=dict(acks=acks,ai_samples=len(ai),ai_cores=sorted({x['core'] for x in ai}),boot=boot,nvs_saved=False)
(base/'results/restore-verification.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r,ensure_ascii=False))
if len(acks)<5:raise SystemExit('Missing restore setting ACKs')
if ai and any(x['core']!=0 for x in ai):raise SystemExit('Unexpected AI core after restore')
