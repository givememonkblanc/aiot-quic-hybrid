import json,random
from pathlib import Path
from PIL import Image
import numpy as np
import torch
from ultralytics import YOLO
root=Path('datasets/person_live/session_01');out=Path('datasets/person_training');out.mkdir(exist_ok=True)
torch.set_num_threads(6);teacher=YOLO('yolo11m.pt');rows=[]
public=Path('datasets/person_public/coco128')
for p in sorted((public/'images/train2017').glob('*.jpg')):
 labels=public/'labels/train2017'/p.with_suffix('.txt').name;objs=[]
 if labels.exists():
  for line in labels.read_text().splitlines():
   c,x,y,w,h=map(float,line.split())
   if int(c)==0:objs.append(dict(cx=x,cy=y,bbox=[x-w/2,y-h/2,x+w/2,y+h/2]))
 rows.append(dict(frame=str(p.resolve()),objects=objs,group='public',source=p.name))
for p in sorted(root.glob('*.jpg')):
 try:
  im=Image.open(p).convert('RGB');im.load()
 except OSError:continue
 # The install scene is rotated; rotate only for teacher, invert label coordinates.
 arr=np.array(im);k=3;r=teacher.predict(np.rot90(arr,k)[:,:,::-1].copy(),classes=[0],conf=.15,device='cpu',verbose=False)[0]
 h,w=r.orig_shape;objs=[];amb=False
 for b in r.boxes:
  conf=float(b.conf[0]);x1,y1,x2,y2=map(float,b.xyxy[0]);cx=(x1+x2)/(2*w);cy=(y1+y2)/(2*h)
  if conf<.45:amb=True;continue
  # np.rot90(...,3): rotated (x,y) = (1-original_y, original_x)
  objs.append(dict(cx=cy,cy=1-cx,confidence=conf))
 if amb and not objs:continue
 # Empty labels from a weak teacher on this partial-person scene are unsafe.
 if not objs:continue
 rows.append(dict(frame=str(p.resolve()),objects=objs,group='local',source=p.name))
 print('local labeled',len(rows)-128,flush=True)
random.Random(41).shuffle(rows[:128])
# Public images are held out by image; local scenes by contiguous time block.
rng=random.Random(41);pub=[r for r in rows if r['group']=='public'];rng.shuffle(pub)
loc=[r for r in rows if r['group']=='local'];split=max(1,int(len(loc)*.75))
for i,r in enumerate(pub):r['split']='val' if i<26 else 'train'
for i,r in enumerate(loc):r['split']='val' if i>=split else 'train'
rows=pub+loc
(out/'labels.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
print(json.dumps({'rows':len(rows),'local_positive':len(loc),'public_person':sum(bool(r['objects']) for r in pub),'train':sum(r['split']=='train' for r in rows),'val':sum(r['split']=='val' for r in rows)}),flush=True)
