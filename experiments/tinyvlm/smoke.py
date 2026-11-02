"""Synthetic plumbing check; never an accuracy benchmark or deployable model."""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image
from train import train
from export import export
from infer import predict


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=False)
    rng=np.random.default_rng(42)
    cache=a.out/'synthetic.npz'
    images=rng.integers(0,256,(4,128,128,3),dtype=np.uint8)
    np.savez(cache,images=images,teacher_image=rng.normal(size=(4,512)).astype('float32'),
        teacher_text=rng.normal(size=(4,512)).astype('float32'),
        class_text=rng.normal(size=(2,4,512)).astype('float32'),classes=np.array(['smoke_a','smoke_b']))
    train(cache,a.out/'run',epochs=1,batch_size=2,smoke=True,accumulation=2)
    info=export(a.out/'run',cache,a.out/'bundle',dim=64,allow_smoke=True)
    results=predict(a.out/'bundle',Image.fromarray(images[0]),min_similarity=-1,min_margin=0,allow_smoke=True)
    assert len(results)==1
    assert info['smoke_only'] and info['dimension']==64
    try:predict(a.out/'bundle',Image.fromarray(images[0]))
    except ValueError:pass
    else:raise AssertionError('smoke bundle must be rejected by normal inference')
    print('PASS: gradient update, fully INT8 conversion, cosine inference, smoke deployment guard')

if __name__=='__main__':main()
