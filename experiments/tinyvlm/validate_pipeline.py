"""Integration test with a real CLIP feature cache; outputs remain smoke-only."""
import argparse
import contextlib
import io
import json
import time
from pathlib import Path
import numpy as np
import tensorflow as tf
from PIL import Image
from model import build_models
from train import train
from export import export,unit
from infer import predict,quantize_rgb,cosine


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    with np.load(args.cache,allow_pickle=False) as f:data={k:f[k] for k in f.files}
    if str(data.get('teacher',''))!='openai/clip-vit-base-patch32':raise ValueError('requires real CLIP-B/32 cache')
    report={'kind':'integration-test-not-accuracy-benchmark','data_pairs':len(data['images']),
            'teacher':str(data['teacher']),'teacher_state_sha256':str(data['teacher_state_sha256']),
            'hardware':'host CPU; no ESP32 execution','exports':[],'guards':[]}
    initial=build_models()[0].layers[-1].get_weights()[0].copy()
    vision,text=train(args.cache,args.out/'run',epochs=3,batch_size=2,smoke=True,accumulation=1)
    report['head_max_weight_change']=float(np.max(np.abs(initial-vision.layers[-1].get_weights()[0])))
    assert report['head_max_weight_change']>0
    report['loss_history']=json.loads((args.out/'run/training.json').read_text())['history']
    for dim in (16,64,256):
        with contextlib.redirect_stdout(io.StringIO()):
            manifest=export(args.out/'run',args.cache,args.out/f'd{dim}',dim,allow_smoke=True)
        bundle=args.out/f'd{dim}'
        interpreter=tf.lite.Interpreter(model_path=str(bundle/'vision_int8.tflite'),num_threads=1)
        interpreter.allocate_tensors();inp=interpreter.get_input_details()[0];out=interpreter.get_output_details()[0]
        assert out['shape'].tolist()==[1,dim] and inp['dtype']==out['dtype']==np.int8
        with np.load(bundle/'prototypes.npz',allow_pickle=False) as f:prototypes=f['values']
        similarities=[];score_errors=[]
        prompts=data['class_text'];k,n,_=prompts.shape
        projected=text(prompts.reshape(-1,512),training=False).numpy().reshape(k,n,256)
        float_proto=unit(unit(projected[:,:,:dim]).mean(axis=1))
        for image in data['images']:
            q=quantize_rgb(image,*inp['quantization']);interpreter.set_tensor(inp['index'],q[None]);interpreter.invoke()
            output=interpreter.get_tensor(out['index'])[0]
            expected=vision(image[None].astype('float32')/127.5-1,training=False).numpy()[0,:dim]
            real=(output.astype('float32')-out['quantization'][1])*out['quantization'][0]
            similarities.append(float(unit(real)@unit(expected)))
            score_errors.append(float(np.max(np.abs(cosine(output,out['quantization'][1],prototypes)-float_proto@unit(expected)))))
        for _ in range(5):interpreter.invoke()
        elapsed=[]
        for _ in range(30):
            start=time.perf_counter_ns();interpreter.invoke();elapsed.append((time.perf_counter_ns()-start)/1e6)
        results=predict(bundle,Image.fromarray(data['images'][0]),regions=True,
                        min_similarity=-1,min_margin=0,allow_smoke=True)
        assert results and all(np.isfinite(r['cosine']) for r in results)
        entry={'dimension':dim,'model_bytes':manifest['model_bytes'],'ops':manifest['ops'],
               'float_int8_embedding_cosine_min':min(similarities),'class_cosine_max_abs_error':max(score_errors),
               'host_single_thread_invoke_ms_p50':float(np.percentile(elapsed,50)),
               'host_single_thread_invoke_ms_p95':float(np.percentile(elapsed,95)),
               'region_forward_count':14,'region_output_count':len(results)}
        report['exports'].append(entry);print(json.dumps(entry),flush=True)
        try:predict(bundle,Image.fromarray(data['images'][0]))
        except ValueError:report['guards'].append(f'd{dim}: normal inference rejects test model')
        else:raise AssertionError('test model accepted')
    for name,call in [
        ('normal export rejects test weights',lambda:export(args.out/'run',args.cache,args.out/'forbidden')),
        ('invalid prefix rejected',lambda:export(args.out/'run',args.cache,args.out/'bad-dim',17,True)),
        ('existing run cannot be overwritten',lambda:train(args.cache,args.out/'run'))]:
        try:call()
        except ValueError:report['guards'].append(name)
        else:raise AssertionError(name)
    (args.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: CLIP-cache training, gradient changes, 3 INT8 dimensions, 14-region inference, guards')
    print(args.out/'report.json')

if __name__=='__main__':main()
