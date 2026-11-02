import json,statistics,sys
from pathlib import Path
p=Path(sys.argv[1]);rows={}
for line in (p/'serial.log').read_text(errors='replace').splitlines():
 for tag in ('BEGIN','LOAD','RUN','BOX','ROI','ROI_CHECK','MEM','DONE'):
  marker='[ESPDET_'+tag+'] '
  if marker in line:
   try:r=json.loads(line.split(marker,1)[1])
   except ValueError:continue
   rows.setdefault(tag,[]).append(r)
def describe(v):
 if not v:return None
 v=sorted(v)
 def q(f):
  x=(len(v)-1)*f;i=int(x);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(x-i)
 return dict(n=len(v),mean=statistics.mean(v),p50=q(.5),p95=q(.95),min=v[0],max=v[-1])
runs=[r for r in rows.get('RUN',[]) if not r['warmup']];roi={r['index']:r for r in rows.get('ROI',[]) if r['index']>=3}
summary={'status':rows.get('DONE',[]),'runs':len(runs),'input':rows.get('BEGIN',[]),'load':rows.get('LOAD',[]),'decode_ms':describe([r['decode_us']/1000 for r in runs]),'detect_ms':describe([r['detect_us']/1000 for r in runs]),'jpeg_to_detection_ms':describe([r['jpeg_to_detection_us']/1000 for r in runs]),'crop_encode_ms':describe([r['crop_encode_us']/1000 for r in roi.values()]),'jpeg_to_roi_phase_sum_ms':describe([(r['jpeg_to_detection_us']+roi[r['index']]['crop_encode_us'])/1000 for r in runs if r['index'] in roi]),'detections':sorted({r['detections'] for r in runs}),'roi_bytes':describe([r['bytes'] for r in roi.values()]),'roi_decode_checks':rows.get('ROI_CHECK',[]),'memory_phases':rows.get('MEM',[])}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='memory_phases'},indent=2))
