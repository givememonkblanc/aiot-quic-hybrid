"""Summarize the fixed binary trial, including frozen CLIP baseline."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np


def metrics(rows):
    tp=sum(r['label']=='person' and r['prediction']=='person' for r in rows)
    tn=sum(r['label']!='person' and r['prediction']!='person' for r in rows)
    fp=sum(r['label']!='person' and r['prediction']=='person' for r in rows)
    fn=sum(r['label']=='person' and r['prediction']!='person' for r in rows)
    return dict(samples=len(rows),correct=tp+tn,accuracy=(tp+tn)/len(rows),
                person_precision=tp/(tp+fp) if tp+fp else None,
                person_recall=tp/(tp+fn) if tp+fn else None,
                confusion=dict(tp=tp,tn=tn,fp=fp,fn=fn))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--trial',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();base=a.trial
    test=json.loads((base/'test_report.json').read_text())
    val=json.loads((base/'validation_report.json').read_text())
    labels=Path(__file__).resolve().parents[2]/'datasets/person_public/coco128/labels/train2017'
    def annotated(r):return (labels/(Path(r['image']).stem+'.txt')).is_file()
    excluded={name:[Path(r['image']).name for r in data['rows'] if not annotated(r)]
              for name,data in [('validation',val),('test',test)]}
    rows=[json.loads(s) for s in (base/'test.jsonl').read_text().splitlines()]
    with np.load(base/'test_cache.npz',allow_pickle=False) as c:
        hashes=[hashlib.sha256((base/r['image']).resolve().read_bytes()).hexdigest() for r in rows]
        if hashes!=c['image_sha256'].tolist():raise ValueError('test cache ordering mismatch')
        prototypes=c['class_text'].mean(axis=1)
        prototypes/=np.linalg.norm(prototypes,axis=-1,keepdims=True)
        predictions=c['classes'][np.argmax(c['teacher_image']@prototypes.T,axis=1)]
        teacher_rows=[dict(label=r['label'],prediction=str(y)) for r,y in zip(rows,predictions) if annotated(r)]
        teacher_hash=str(c['teacher_state_sha256'])
    report=dict(task='COCO128 person presence, closed-candidate binary image classification',
        limitations=['Independent reconstruction, not an official TinyVLM checkpoint',
                    'ImageNet initialization and 78 labelled training images; not a zero-shot evaluation',
                    'Only two label-derived captions; small single-seed trial',
                    'Exact-byte duplicates excluded; broader near-duplicate/pretraining overlap not audited',
                    'No localization AP, camera domain evaluation, or ESP32 runtime measurement'],
        split=json.loads((base/'split.json').read_text()),
        excluded_missing_annotation=excluded,
        initial_unverified_validation=metrics(val['rows']),initial_unverified_test=metrics(test['rows']),
        validation=metrics([r for r in val['rows'] if annotated(r)]),
        test=metrics([r for r in test['rows'] if annotated(r)]),
        clip_teacher_test=metrics(teacher_rows),teacher_state_sha256=teacher_hash,
        bundle=json.loads((base/'bundle64/manifest.json').read_text()),
        hardware_status='not deployed; awaiting confirmation of temporary firmware test')
    a.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('validation','test','clip_teacher_test')},indent=2))


if __name__=='__main__':main()
