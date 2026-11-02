import json
from pathlib import Path
import numpy as np
from PIL import Image
import cv2
import tensorflow as tf
out=Path('experiments/person_detection/out');rows=[json.loads(s) for s in Path('datasets/person_training/labels.jsonl').read_text().splitlines() if json.loads(s)['split']=='val']
tf.config.threading.set_intra_op_parallelism_threads(4)
i=tf.lite.Interpreter(model_path=str(out/'person_int8.tflite'),num_threads=4);i.allocate_tensors();a=i.get_input_details()[0];b=i.get_output_details()[0];pred=[]
for r in rows:
 x=np.asarray(Image.open(r['frame']).convert('L').resize((96,96),Image.Resampling.NEAREST),np.float32)/255;s,z=a['quantization'];i.set_tensor(a['index'],np.clip(np.rint(x/s+z),-128,127).astype(np.int8)[None,...,None]);i.invoke();s,z=b['quantization'];pred.append((i.get_tensor(b['index'])[0,...,1].astype(float)-z)*s)
reports=[]
for threshold in [.3,.4,.5,.6,.7,.8,.9]:
 for group in ['all','local','public']:
  tp=fp=fn=0
  for r,heat in zip(rows,pred):
   if group!='all' and r['group']!=group:continue
   n,lab,stats,centers=cv2.connectedComponentsWithStats((heat>=threshold).astype(np.uint8),8)
   # Match firmware: component bounding-rectangle center in grid coordinates.
   points=[((v[0]+v[2]/2)/12,(v[1]+v[3]/2)/12) for v in stats[1:]];truth=[(o['cx'],o['cy']) for o in r['objects']]
   pairs=sorted((np.hypot(p[0]-t[0],p[1]-t[1]),j,k) for j,p in enumerate(points) for k,t in enumerate(truth));usedp=set();usedt=set()
   for d,j,k in pairs:
    if d<=1.5/12 and j not in usedp and k not in usedt:usedp.add(j);usedt.add(k)
   tp+=len(usedp);fp+=len(points)-len(usedp);fn+=len(truth)-len(usedt)
  precision=tp/max(1,tp+fp);recall=tp/max(1,tp+fn);reports.append(dict(threshold=threshold,group=group,tp=tp,fp=fp,fn=fn,precision=precision,recall=recall,f1=2*precision*recall/max(1e-9,precision+recall)))
(out/'cluster-validation.json').write_text(json.dumps(reports,indent=2));print(json.dumps(reports))
