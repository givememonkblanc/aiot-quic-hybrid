"""Join ESP32 FOMO results with the exact transmitted frame; keep latest snapshot only."""
import io,json,struct,time
from collections import OrderedDict,deque
from pathlib import Path
from PIL import Image,ImageDraw
ROOT=Path(__file__).resolve().parents[2]
feed=ROOT/'experiments/handoff/results/cross-ap-final-mode2-03/feed.jsonl'
out=ROOT/'experiments/person_detection/results';out.mkdir(exist_ok=True)
frames=OrderedDict();results=OrderedDict();buf=b'';key=None;nextseq=0
frame_times=deque();ai_times=deque();laststatus=lastshot=0

def atomic(name,data):
 p=out/name;tmp=p.with_suffix(p.suffix+'.tmp');tmp.write_bytes(data);tmp.replace(p)

def match(k):
 global lastshot
 if k not in frames or k not in results:return
 d=results[k]
 if not d['detections']:return
 raw=frames[k]
 try:
  with Image.open(io.BytesIO(raw)) as im:im.load();canvas=im.convert('RGB')
 except OSError:return
 draw=ImageDraw.Draw(canvas)
 for det in d['detections']:
  x,y,w,h=det['cell_box'];cx=(x+w/2)*canvas.width/255;cy=(y+h/2)*canvas.height/255
  draw.ellipse((cx-9,cy-9,cx+9,cy+9),outline='lime',width=3)
  draw.text((max(0,cx-35),max(0,cy-25)),f"person {det['confidence']:.0%}",fill='lime')
 b=io.BytesIO();canvas.save(b,'JPEG',quality=90)
 atomic('person-original.jpg',raw);atomic('person-marked.jpg',b.getvalue())
 lastshot=time.time();d.update(cid=k[0],frame_ts_ms=k[1],received_epoch=lastshot,model='ESP32 FOMO prototype',annotation='centroid, not full person bounding box')
 atomic('snapshot.json',json.dumps(d,indent=2).encode())
 frames.pop(k,None);results.pop(k,None)

with feed.open('rb') as f:
 f.seek(0,2);pending=b''
 while True:
  chunk=f.read(65536)
  if chunk:
   pending+=chunk;lines=pending.split(b'\n');pending=lines.pop()
   for line in lines:
    try:r=json.loads(line.decode('latin1'))
    except ValueError:continue
    if r.get('kind')!='datagram' or r.get('track') not in (2,3):continue
    data=r['payload'].encode('latin1');k=(r['cid'],r['gen_ts_ms']);now=time.time()
    if r['track']==3:
     start=data.startswith(b'\xff\xd8')
     if start:buf=b'';key=k;nextseq=r['seq']
     if (not start and not buf) or k!=key or r['seq']!=nextseq or len(buf)+len(data)>262144:buf=b'';continue
     buf+=data;nextseq=(r['seq']+1)&0xffffffff
     if buf.endswith(b'\xff\xd9'):
      try:
       with Image.open(io.BytesIO(buf)) as im:im.load();dims=im.size
      except OSError:buf=b'';continue
      frame_times.append(now)
      if dims in ((640,640),(1024,1024)):frames[k]=buf;match(k)
      buf=b''
    elif len(data)>=20 and data[0]==0xd1 and data[1]>=2 and data[2]==2:
     sw,sh,iw,ih,seq=struct.unpack_from('<HHHHI',data,4)
     if (sw,sh) not in ((640,640),(1024,1024)):continue
     ai_times.append(now);dets=[]
     for j in range(data[3]):
      off=20+j*6
      if off+6>len(data):break
      cls,conf,x,y,w,h=data[off:off+6]
      if cls==1:dets.append(dict(confidence=conf/255,cell_box=[x,y,w,h]))
     results[k]=dict(detections=dets,source_sequence=seq,raw_score=data[16]);match(k)
    while len(frames)>120:frames.popitem(last=False)
    while len(results)>120:results.popitem(last=False)
  else:time.sleep(.01)
  now=time.time()
  if now-laststatus>2:
   for q in (frame_times,ai_times):
    while q and q[0]<now-10:q.popleft()
   metrics=dict(received_epoch=now,valid_frame_fps_10s=len(frame_times)/10,ai_result_fps_10s=len(ai_times)/10,last_snapshot_epoch=lastshot or None)
   atomic('rates.json',json.dumps(metrics).encode());laststatus=now
