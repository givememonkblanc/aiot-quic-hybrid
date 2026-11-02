import os,select,time,tty,json
from pathlib import Path
p=Path('experiments/capture10/results');fd=os.open('/dev/ttyACM0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK);tty.setraw(fd)
raw=(p/'initial-serial.log').open('wb');buf=b'';end=time.monotonic()+12
try:
 while time.monotonic()<end:
  if not select.select([fd],[],[],.2)[0]:continue
  b=os.read(fd,65536);raw.write(b);buf+=b
  while b'\n' in buf:
   line,buf=buf.split(b'\n',1);s=line.decode(errors='replace')
   if any(t in s for t in ['[AI_CPU]','[AI_PHASE]','[AI_PROFILE]','[PROFILE]','[DGRAM_PERF]','event=camera_frame']):print(s)
finally:raw.close();os.close(fd)
