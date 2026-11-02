"""Generate four deterministic held-out inputs for the isolated ESP32 build."""
import argparse
import json
import shutil
from pathlib import Path
import numpy as np
from PIL import Image


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--trial',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    rows=[json.loads(x) for x in (a.trial/'test.jsonl').read_text().splitlines()]
    chosen=[]
    for label in ['person','background scene']:
        group=[r for r in rows if r['label']==label][:2]
        if len(group)!=2:raise ValueError('need two held-out images of each class')
        chosen.extend(group)
    samples=[]
    for r in chosen:
        with Image.open(a.trial/r['image']) as im:
            samples.append(np.asarray(im.convert('RGB').resize((128,128),Image.Resampling.BILINEAR)))
    shutil.copy(a.trial/'bundle64/tinyvlm_assets.h',a.out/'tinyvlm_assets.h')
    (a.out/'samples.h').write_text('#pragma once\n#include <stdint.h>\nalignas(16) static const uint8_t samples[4][49152]={\n'+
        ',\n'.join('{'+','.join(map(str,s.flatten().tolist()))+'}' for s in samples)+'\n};\n')
    (a.trial/'hardware_samples.json').write_text(json.dumps(chosen,indent=2)+'\n')


if __name__=='__main__':main()
