"""Export a trained vision encoder and projected class prototypes to INT8."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import tensorflow as tf
from model import DIMS


def unit(x):
    norm=np.linalg.norm(x,axis=-1,keepdims=True)
    if np.any(norm<1e-10) or not np.isfinite(x).all(): raise ValueError('invalid/zero embedding')
    return x/norm


def export(run,cache,out,dim=64,allow_smoke=False):
    if dim not in DIMS: raise ValueError('unsupported Matryoshka dimension')
    if out.exists(): raise ValueError('output directory exists')
    metadata=json.loads((run/'training.json').read_text())
    if metadata['smoke_only'] and not allow_smoke: raise ValueError('smoke checkpoint is not a deployable model')
    if hashlib.sha256(cache.read_bytes()).hexdigest()!=metadata['cache_sha256']: raise ValueError('cache/checkpoint mismatch')
    vision=tf.keras.models.load_model(run/'vision.keras',compile=False)
    text=tf.keras.models.load_model(run/'text.keras',compile=False)
    with np.load(cache,allow_pickle=False) as f:
        images=f['images'];classes=f['classes'];teacher_prompts=f['class_text']
    k,prompts,_=teacher_prompts.shape
    e=text(teacher_prompts.reshape(-1,512),training=False).numpy().reshape(k,prompts,256)
    # Prefix first, normalize each prompt, average, normalize per class.
    prototypes=unit(unit(e[:,:,:dim]).mean(axis=1))
    scales=np.max(np.abs(prototypes),axis=1)/127
    quantized=np.clip(np.rint(prototypes/scales[:,None]),-127,127).astype(np.int8)
    inputs=tf.keras.Input((128,128,3),batch_size=1,dtype=tf.float32)
    head=tf.keras.layers.Dense(dim, name='deployment_embedding')
    outputs=head(vision.layers[0](inputs,training=False))
    encoder=tf.keras.Model(inputs,outputs)
    kernel,bias=vision.layers[-1].get_weights()
    head.set_weights([kernel[:,:dim],bias[:dim]])
    # Freeze variables explicitly; avoids Keras 3 / legacy converter mismatch.
    from tensorflow.python.framework.convert_to_constants import convert_variables_to_constants_v2
    function=tf.function(lambda x: encoder(x,training=False),
                         input_signature=[tf.TensorSpec([1,128,128,3],tf.float32)])
    frozen=convert_variables_to_constants_v2(function.get_concrete_function())
    converter=tf.lite.TFLiteConverter.from_concrete_functions([frozen])
    converter.optimizations=[tf.lite.Optimize.DEFAULT]
    converter.representative_dataset=lambda: ([x[None].astype('float32')/127.5-1] for x in images[:256])
    converter.target_spec.supported_ops=[tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type=tf.int8;converter.inference_output_type=tf.int8
    binary=converter.convert()
    interpreter=tf.lite.Interpreter(model_content=binary);interpreter.allocate_tensors()
    inp=interpreter.get_input_details()[0];output=interpreter.get_output_details()[0]
    if inp['dtype']!=np.int8 or output['dtype']!=np.int8: raise ValueError('INT8 conversion failed')
    out.mkdir(parents=True)
    (out/'vision_int8.tflite').write_bytes(binary)
    np.savez(out/'prototypes.npz',values=quantized,scales=scales,classes=classes,dim=dim)
    def array(name,values,ctype):
        flat=values.reshape(-1)
        return f'TINYVLM_ALIGN static const {ctype} {name}[] = {{\n'+',\n'.join(','.join(str(int(x)) for x in flat[i:i+32]) for i in range(0,len(flat),32))+'\n};\n'
    (out/'tinyvlm_assets.h').write_text('#pragma once\n#include <stdint.h>\n#if defined(__cplusplus)\n#define TINYVLM_ALIGN alignas(16)\n#else\n#define TINYVLM_ALIGN _Alignas(16)\n#endif\n'+
        f'#define TINYVLM_DIM {dim}\n#define TINYVLM_CLASSES {k}\n'+
        array('tinyvlm_model',np.frombuffer(binary,dtype=np.uint8),'uint8_t')+
        array('tinyvlm_prototypes',quantized,'int8_t'))
    info=dict(paper='2603.00136v1',implementation='independent-reconstruction',smoke_only=metadata['smoke_only'],
        input_shape=inp['shape'].tolist(),input_scale=inp['quantization'][0],input_zero_point=inp['quantization'][1],
        output_scale=output['quantization'][0],output_zero_point=output['quantization'][1],
        dimension=dim,classes=classes.tolist(),model_bytes=len(binary),prototype_bytes=quantized.nbytes,
        model_sha256=hashlib.sha256(binary).hexdigest(),prototype_sha256=hashlib.sha256(quantized.tobytes()).hexdigest(),
        normalization='RGB / 127.5 - 1',resize='bilinear 128x128',
        ops=sorted({x['op_name'] for x in interpreter._get_ops_details() if x['op_name']!='DELEGATE'}),
        training_cache_sha256=metadata['cache_sha256'],
        accuracy_validated=False,esp32_latency_ms=None,tensor_arena_bytes=None)
    (out/'manifest.json').write_text(json.dumps(info,indent=2)+'\n')
    print(json.dumps(info,indent=2))
    return info


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('run','cache','out'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--dim',type=int,choices=DIMS,default=64)
    a=p.parse_args();export(a.run,a.cache,a.out,a.dim)

if __name__=='__main__':main()
