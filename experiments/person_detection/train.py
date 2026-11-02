"""Train an int8 person-centroid FOMO prototype; image/time-block split before augmentation."""
import os
os.environ.setdefault('TF_NUM_INTRAOP_THREADS','8');os.environ.setdefault('TF_NUM_INTEROP_THREADS','2')
import json,sys,importlib.util
from pathlib import Path
import numpy as np
from PIL import Image
import tensorflow as tf
spec=importlib.util.spec_from_file_location('recipe','experiments/vehicle_count/train_fomo_tl.py');recipe=importlib.util.module_from_spec(spec);spec.loader.exec_module(recipe)
recipe.INPUT=96
tf.keras.utils.set_random_seed(41)
rows=[json.loads(x) for x in Path('datasets/person_training/labels.jsonl').read_text().splitlines()]
out=Path('experiments/person_detection/out');out.mkdir(exist_ok=True)
inp0=tf.keras.Input((96,96,1),name='input_luma')
rgb=tf.keras.layers.Concatenate()([inp0,inp0,inp0])
rgb=tf.keras.layers.Rescaling(2.,offset=-1.)(rgb)
base=tf.keras.applications.MobileNetV2(input_tensor=rgb,weights='imagenet',alpha=.35,include_top=True)
base.trainable=False
z=base.get_layer('block_6_expand_relu').output
z=tf.keras.layers.Conv2D(32,1,activation='relu',name='fomo_head1')(z)
z=tf.keras.layers.Conv2D(2,1,name='fomo_logits')(z)
z=tf.keras.layers.Softmax(name='fomo_out')(z)
model=tf.keras.Model(inp0,z);grid=int(model.output_shape[1]);sets={}
for split in ['train','val']:
 xs=[];ys=[]
 for row in rows:
  if row['split']!=split:continue
  x=np.asarray(Image.open(row['frame']).convert('L').resize((96,96),Image.Resampling.NEAREST),dtype=np.float32)/255
  y=np.zeros((grid,grid),np.float32)
  for o in row['objects']:y[min(grid-1,int(o['cy']*grid)),min(grid-1,int(o['cx']*grid))]=1
  xs.append(x);ys.append(y)
  if split=='train':xs.append(x[:,::-1]);ys.append(y[:,::-1])
 sets[split]=(np.stack(xs)[...,None],np.stack(ys))
x,y=sets['train'];xv,yv=sets['val'];oh=lambda y:np.stack([1-y,y],axis=-1)
features=tf.keras.Model(model.input,base.get_layer('block_6_expand_relu').output)
ft=features.predict(x,batch_size=32,verbose=0);fv=features.predict(xv,batch_size=32,verbose=0)
a=tf.keras.Input(ft.shape[1:]);head=tf.keras.Model(a,model.get_layer('fomo_out')(model.get_layer('fomo_logits')(model.get_layer('fomo_head1')(a))))
weight=min(50.,max(5.,(1-y.mean())/max(y.mean(),.001)/2))
def loss(t,p):return tf.reduce_mean(-tf.reduce_sum(t*tf.math.log(tf.clip_by_value(p,1e-7,1))*tf.constant([1.,weight]),axis=-1))
head.compile(optimizer=tf.keras.optimizers.Adam(.001),loss=loss)
head.fit(ft,oh(y),validation_data=(fv,oh(yv)),epochs=60,batch_size=32,verbose=2,callbacks=[tf.keras.callbacks.EarlyStopping(patience=12,restore_best_weights=True)])
# Adapt the final backbone blocks to the person task while freezing BN stats.
for layer in model.layers:
    if layer.name.startswith(('block_4', 'block_5', 'block_6')) and not isinstance(layer, tf.keras.layers.BatchNormalization):
        layer.trainable = True
model.compile(optimizer=tf.keras.optimizers.Adam(.0001),loss=loss)
model.fit(x,oh(y),validation_data=(xv,oh(yv)),epochs=40,batch_size=16,verbose=2,
          callbacks=[tf.keras.callbacks.EarlyStopping(patience=10,restore_best_weights=True)])
# Shared layers put the trained head directly into the full model.
model.save_weights(out/"person.weights.h5")
recipe.export_tflite(model,x[...,0],out)
tfl=out/'fomo_vehicle_int8.tflite';tfl.rename(out/'person_int8.tflite')
inter=tf.lite.Interpreter(model_path=str(out/'person_int8.tflite'),num_threads=4);inter.allocate_tensors();inp=inter.get_input_details()[0];op=inter.get_output_details()[0]
pred=[]
for image in xv:
 scale,zero=inp['quantization'];v=np.clip(np.rint(image/scale+zero),-128,127).astype(np.int8)[None];inter.set_tensor(inp['index'],v);inter.invoke();scale,zero=op['quantization'];pred.append((inter.get_tensor(op['index'])[0].astype(np.float32)-zero)*scale)
pred=np.stack(pred);scores=[]
for threshold in [.3,.4,.5,.6,.7,.8]:
 # Strict cell metric; do not present this as detector mAP or real-world accuracy.
 p=pred[...,1]>=threshold;t=yv>0;tp=int((p&t).sum());fp=int((p&~t).sum());fn=int((~p&t).sum());precision=tp/max(tp+fp,1);recall=tp/max(tp+fn,1)
 scores.append(dict(threshold=threshold,precision=precision,recall=recall,f1=2*precision*recall/max(precision+recall,1e-9),tp=tp,fp=fp,fn=fn))
best=max(scores,key=lambda v:v['f1']);report=dict(train_images=len(x)//2,val_images=len(xv),grid=grid,scores=scores,selected=best,scope='prototype; small public data + local rotated-teacher positives; no independent test set')
(out/'validation.json').write_text(json.dumps(report,indent=2));print(json.dumps(report),flush=True)
# A separate asset preserves the existing parking model and its user's changes.
size=(out/'person_int8.tflite').stat().st_size
(out/'person_asset.h').write_text('''#pragma once
#include "ai_inference_model_data.h"
extern const unsigned char g_vehicle_fomo_model_data[];
static const ai_inference_model_asset_t g_ai_inference_generated_model_asset = {
.data=g_vehicle_fomo_model_data, .len=%dU, .tensor_arena_size=384U*1024U,
.input_width=96U, .input_height=96U, .detection_threshold=%dU,
.class_count=1U, .background_index=0U, .target_class_index=0U,
.output_type=AI_INFERENCE_OUTPUT_FOMO,
};
'''%(size,round(best['threshold']*255)))
