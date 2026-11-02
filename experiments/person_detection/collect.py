import argparse,hashlib,io,json,time
from pathlib import Path
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('--seconds',type=int,default=300);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=True);src=Path('experiments/handoff/results/cross-ap-final-mode2-03/latest.jpg');end=time.monotonic()+a.seconds;seen=set()
with (a.out/'manifest.jsonl').open('a') as f:
 while time.monotonic()<end:
  try:
   b=src.read_bytes();sha=hashlib.sha256(b).hexdigest()
   if sha not in seen and time.time()-src.stat().st_mtime<5:
    with Image.open(io.BytesIO(b)) as im:im.load();w,h=im.size
    name=f'{time.time_ns()}.jpg';(a.out/name).write_bytes(b);seen.add(sha)
    f.write(json.dumps(dict(frame=name,sha256=sha,width=w,height=h,received_epoch=time.time()))+'\n');f.flush()
  except (OSError,ValueError):pass
  time.sleep(.1)
print('captured',len(seen),flush=True)
