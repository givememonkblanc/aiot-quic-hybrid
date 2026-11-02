"""INT8 prototype classification; optional explicit grid-proposal reconstruction.

The paper does not specify its proposal generator. --regions is an independent
multi-scale grid baseline, NOT the paper's detection head or claimed AP.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image
from export import unit


def cosine(output,zero_point,prototypes):
    return unit(prototypes.astype('float32')) @ unit(output.astype('float32')-zero_point)


def quantize_rgb(rgb,scale,zero_point):
    values=(rgb.astype('float32')/np.float32(127.5)-np.float32(1))/np.float32(scale)
    rounded=np.sign(values)*np.floor(np.abs(values)+np.float32(.5))
    return np.clip(rounded+zero_point,-128,127).astype('int8')


def proposals(width,height):
    yield (0,0,width,height)
    for divisions in (2,3):
        for y in range(divisions):
            for x in range(divisions):
                yield (x*width//divisions,y*height//divisions,(x+1)*width//divisions,(y+1)*height//divisions)


def iou(a,b):
    intersection=max(0,min(a[2],b[2])-max(a[0],b[0]))*max(0,min(a[3],b[3])-max(a[1],b[1]))
    union=(a[2]-a[0])*(a[3]-a[1])+(b[2]-b[0])*(b[3]-b[1])-intersection
    return intersection/union if union else 0


def predict(bundle,image,regions=False,min_similarity=.3,min_margin=.05,allow_smoke=False):
    import tensorflow as tf
    manifest=json.loads((bundle/'manifest.json').read_text())
    if manifest['smoke_only'] and not allow_smoke:raise ValueError('smoke-only bundle; not a trained detector')
    interpreter=tf.lite.Interpreter(model_path=str(bundle/'vision_int8.tflite'));interpreter.allocate_tensors()
    inp=interpreter.get_input_details()[0];out=interpreter.get_output_details()[0]
    with np.load(bundle/'prototypes.npz',allow_pickle=False) as f:proto=f['values'];classes=f['classes']
    image=image.convert('RGB');found=[]
    for box in proposals(*image.size) if regions else [(0,0,*image.size)]:
        rgb=np.asarray(image.crop(box).resize((128,128),Image.Resampling.BILINEAR))
        scale,zp=inp['quantization'];x=quantize_rgb(rgb,scale,zp)[None]
        interpreter.set_tensor(inp['index'],x);interpreter.invoke()
        scores=cosine(interpreter.get_tensor(out['index'])[0],out['quantization'][1],proto)
        order=np.argsort(-scores);best=int(order[0]);margin=float(scores[best]-scores[order[1]]) if len(order)>1 else 0.
        if scores[best]<min_similarity or margin<min_margin:continue
        candidate=dict(label=str(classes[best]),cosine=float(scores[best]),margin=margin,box=list(box))
        found.append(candidate)
    keep=[]
    for candidate in sorted(found,key=lambda x:-x['cosine']):
        if not any(k['label']==candidate['label'] and iou(k['box'],candidate['box'])>.5 for k in keep):keep.append(candidate)
    return keep


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--bundle',type=Path,required=True);p.add_argument('--image',type=Path,required=True)
    p.add_argument('--regions',action='store_true');p.add_argument('--min-similarity',type=float,default=.3);p.add_argument('--min-margin',type=float,default=.05)
    a=p.parse_args()
    with Image.open(a.image) as im:result=predict(a.bundle,im,a.regions,a.min_similarity,a.min_margin)
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
