"""Train the reconstructed student on a prepared CLIP feature cache."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import numpy as np
import tensorflow as tf
from model import build_models, objective


def train(cache, out, epochs=100, batch_size=32, seed=42, smoke=False, accumulation=8, backbone_weights=None):
    if out.exists(): raise ValueError('output directory already exists')
    with np.load(cache,allow_pickle=False) as f: data={k:f[k] for k in f.files}
    images=data['images'];n=len(images)
    if images.shape!=(n,128,128,3) or images.dtype!=np.uint8: raise ValueError('images must be uint8 [N,128,128,3]')
    for key in ('teacher_image','teacher_text'):
        if data[key].shape!=(n,512) or not np.isfinite(data[key]).all(): raise ValueError(f'invalid {key}')
    if n<2 or batch_size<2 or epochs<1 or accumulation<1: raise ValueError('invalid training size')
    vision,text,projection=build_models(seed,backbone_weights)
    optimizer=tf.keras.optimizers.AdamW(learning_rate=.001,weight_decay=.01)
    variables=vision.trainable_variables+text.trainable_variables+projection.trainable_variables
    rng=np.random.default_rng(seed);out.mkdir(parents=True)
    @tf.function
    def step(x,ti,tt):
        with tf.GradientTape() as tape:
            loss,parts=objective(vision(x,training=True),text(tt,training=True),ti,tt,projection)
        grads=tape.gradient(loss,variables)
        return loss,parts,grads
    history=[]
    for epoch in range(epochs):
        lr=.001*min(1.,(epoch+1)/10)*(.5*(1+math.cos(math.pi*max(0,epoch-10)/max(1,epochs-10))))
        optimizer.learning_rate.assign(lr);order=rng.permutation(n);values=[]
        accumulated=[tf.zeros_like(v) for v in variables];microbatches=0
        for start in range(0,n,batch_size):
            idx=order[start:start+batch_size]
            if len(idx)<2: continue
            loss,parts,grads=step(images[idx].astype('float32')/127.5-1,data['teacher_image'][idx],data['teacher_text'][idx])
            value=float(loss.numpy())
            if not math.isfinite(value): raise ValueError('non-finite training loss')
            values.append(value)
            accumulated=[a+g for a,g in zip(accumulated,grads)];microbatches+=1
            if microbatches==accumulation:
                optimizer.apply_gradients((g/microbatches,v) for g,v in zip(accumulated,variables))
                accumulated=[tf.zeros_like(v) for v in variables];microbatches=0
        if microbatches:
            optimizer.apply_gradients((g/microbatches,v) for g,v in zip(accumulated,variables))
        record=dict(epoch=epoch+1,loss=float(np.mean(values)),lr=lr)
        history.append(record);print(json.dumps(record),flush=True)
    vision.save(out/'vision.keras');text.save(out/'text.keras');projection.save(out/'teacher_projection.keras')
    metadata=dict(implementation='independent-paper-reconstruction',paper='2603.00136v1',
        smoke_only=smoke,epochs=epochs,batch_size=batch_size,seed=seed,pairs=n,
        cache_sha256=hashlib.sha256(cache.read_bytes()).hexdigest(),tensorflow=tf.__version__,
        contrastive_batch_size=batch_size,gradient_accumulation=accumulation,history=history,
        backbone_initialization=backbone_weights or 'random',accuracy_validated=False)
    (out/'training.json').write_text(json.dumps(metadata,indent=2)+'\n')
    return vision,text


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--epochs',type=int,default=100);p.add_argument('--batch-size',type=int,default=32);p.add_argument('--seed',type=int,default=42)
    p.add_argument('--accumulation',type=int,default=8)
    p.add_argument('--backbone-weights',choices=['imagenet'],default=None)
    a=p.parse_args();train(a.cache,a.out,a.epochs,a.batch_size,a.seed,accumulation=a.accumulation,backbone_weights=a.backbone_weights)

if __name__=='__main__':main()
