"""Deterministic real-image person/background trial from local COCO128 labels."""
import hashlib,json,random
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
public=ROOT/'datasets/person_public/coco128'
out=ROOT/'experiments/tinyvlm/person_trial'
if out.exists():raise SystemExit('person_trial exists; refusing overwrite')
out.mkdir()
classes=['person','background scene']
rows=[];seen=set()
for image in sorted((public/'images/train2017').glob('*.jpg')):
    sha=hashlib.sha256(image.read_bytes()).hexdigest()
    if sha in seen:continue
    seen.add(sha)
    label=public/'labels/train2017'/image.with_suffix('.txt').name
    # Missing YOLO files may mean unannotated data; exclude from scored sets.
    objects=[int(line.split()[0]) for line in label.read_text().splitlines() if line.strip()] if label.exists() else []
    person=0 in objects
    with Image.open(image) as im: im.verify()
    rows.append(dict(image=str(image),label=classes[0 if person else 1],image_sha256=sha,
        annotation_present=label.exists(),
        caption='a photo of a person in a scene' if person else 'a photo of a background scene without people',
        person_count=objects.count(0)))
rng=random.Random(42);splits={k:[] for k in ['train','validation','test']}
for label in classes:
    group=[r for r in rows if r['label']==label];rng.shuffle(group)
    n=len(group);validation=max(1,round(n*.2));test=max(1,round(n*.2))
    for name,part in [('validation',group[:validation]),('test',group[validation:validation+test]),('train',group[validation+test:])]:splits[name]+=part
for name,part in splits.items():
    rng.shuffle(part);(out/(name+'.jsonl')).write_text(''.join(json.dumps(r)+'\n' for r in part))
(out/'classes.json').write_text(json.dumps(classes))
summary={k:{'total':len(v),'person':sum(r['label']=='person' for r in v),'background':sum(r['label']!='person' for r in v)} for k,v in splits.items()}
(out/'split.json').write_text(json.dumps(summary,indent=2)+'\n');print(summary)
