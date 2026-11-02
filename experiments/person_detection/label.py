"""Local teacher labeling; no frames are uploaded to a third-party service."""
import argparse,json,time
from pathlib import Path
import torch
from ultralytics import YOLO
p=argparse.ArgumentParser();p.add_argument('--frames',type=Path,required=True);p.add_argument('--seconds',type=int,default=300);a=p.parse_args()
torch.set_num_threads(6)
model=YOLO('experiments/accuracy/yolo11n.pt')
seen=set();end=time.monotonic()+a.seconds
with (a.frames/'labels.jsonl').open('a') as out:
 while time.monotonic()<end:
  for f in sorted(a.frames.glob('*.jpg')):
   if f.name in seen:continue
   results=model.predict(str(f),classes=[0],conf=.15,imgsz=640,device='cpu',verbose=False)[0]
   h,w=results.orig_shape;boxes=[];ambiguous=False
   for b in results.boxes:
    conf=float(b.conf[0]);x1,y1,x2,y2=map(float,b.xyxy[0])
    if conf<.45:ambiguous=True;continue
    boxes.append(dict(cx=(x1+x2)/(2*w),cy=(y1+y2)/(2*h),bbox=[x1/w,y1/h,x2/w,y2/h],confidence=conf))
   out.write(json.dumps(dict(frame=f.name,objects=boxes,ambiguous=ambiguous,teacher='yolo11n',width=w,height=h))+'\n');out.flush();seen.add(f.name)
  time.sleep(.5)
print('labeled',len(seen))
