"""Held-out image classification evaluation; does not report detection AP."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image
from infer import predict


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','manifest','training-cache','out'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if a.out.exists():p.error('report already exists')
    model=json.loads((a.bundle/'manifest.json').read_text())
    if model['smoke_only']:p.error('cannot evaluate synthetic smoke model as trained model')
    if hashlib.sha256(a.training_cache.read_bytes()).hexdigest()!=model['training_cache_sha256']:
        p.error('training cache does not match bundle provenance')
    with np.load(a.training_cache,allow_pickle=False) as f:training=set(f['image_sha256'].tolist())
    rows=[]
    for line in a.manifest.read_text().splitlines():
        if not line.strip():continue
        r=json.loads(line);path=(a.manifest.parent/r['image']).resolve()
        if r['label'] not in model['classes']:raise ValueError('label outside candidate classes')
        if hashlib.sha256(path.read_bytes()).hexdigest() in training:raise ValueError('evaluation image overlaps training')
        with Image.open(path) as im:result=predict(a.bundle,im,min_similarity=-1,min_margin=0)
        rows.append(dict(image=r['image'],label=r['label'],prediction=result[0]['label'] if result else None))
    if not rows:raise ValueError('empty evaluation set')
    report=dict(metric='closed-candidate top-1 image accuracy, not detection AP',samples=len(rows),
        accuracy=sum(r['label']==r['prediction'] for r in rows)/len(rows),
        model_sha256=model['model_sha256'],manifest_sha256=hashlib.sha256(a.manifest.read_bytes()).hexdigest(),rows=rows)
    a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='rows'},indent=2))

if __name__=='__main__':main()
