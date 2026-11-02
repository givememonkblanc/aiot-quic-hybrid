#!/usr/bin/env python3
"""Evaluate the exported int8 FOMO model and retain every held-out frame."""
import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image
import tensorflow as tf


def components(heat, threshold):
    _, _, _, centers = cv2.connectedComponentsWithStats(
        (heat >= threshold).astype(np.uint8), 8)
    return [(float(x)/12, float(y)/12) for x, y in centers[1:]]


def evaluate(records, inference, threshold, details=False):
    result=[];tp=fp=fn=0
    for row in records:
        heat=inference(row);pred=components(heat,threshold);truth=[(o["cx"],o["cy"]) for o in row["objects"]]
        pairs=sorted((np.hypot(px-tx,py-ty),i,j) for i,(px,py) in enumerate(pred) for j,(tx,ty) in enumerate(truth))
        usedp=set();usedt=set()
        for distance,i,j in pairs:
            if distance<=2/12 and i not in usedp and j not in usedt:usedp.add(i);usedt.add(j)
        rtp=len(usedt);rfp=len(pred)-len(usedp);rfn=len(truth)-len(usedt)
        tp+=rtp;fp+=rfp;fn+=rfn
        if details:
            result.append(dict(frame=row["frame"],screw_type=row["screw_type"],gt=len(truth),
                               predicted=len(pred),tp=rtp,fp=rfp,fn=rfn,max_score=float(heat.max())))
    precision=tp/max(tp+fp,1);recall=tp/max(tp+fn,1)
    return dict(threshold=float(threshold),gt_screws=tp+fn,detected=tp,missed=fn,
                false_positives=fp,precision=precision,recall=recall,
                f1=2*precision*recall/max(precision+recall,1e-9),details=result)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model", type=Path, default=Path("experiments/screw_mission/out/screw_fomo_int8.tflite"))
    p.add_argument("--labels", type=Path, default=Path("datasets/screw_training/public-labels.jsonl"))
    p.add_argument("--training-report", type=Path, default=Path("experiments/screw_mission/out/public-validation.json"))
    p.add_argument("--split", default="public_test")
    p.add_argument("--out", type=Path, default=Path("experiments/screw_mission/results/public-int8.json"))
    a = p.parse_args()
    rows = [json.loads(x) for x in a.labels.read_text().splitlines() if x.strip()]
    validation = [r for r in rows if r["split"] == "val"]
    test = [r for r in rows if r["split"] == a.split]
    interpreter = tf.lite.Interpreter(model_path=str(a.model), num_threads=4)
    interpreter.allocate_tensors(); inp=interpreter.get_input_details()[0]; out=interpreter.get_output_details()[0]
    cache={}
    def inference(row):
        if row["frame"] in cache:return cache[row["frame"]]
        x=np.asarray(Image.open(row["frame"]).convert("L").resize((96,96),Image.Resampling.BILINEAR),np.float32)/255
        scale,zero=inp["quantization"]; q=np.clip(np.rint(x/scale+zero),-128,127).astype(np.int8)[None,...,None]
        interpreter.set_tensor(inp["index"],q);interpreter.invoke()
        scale,zero=out["quantization"];heat=(interpreter.get_tensor(out["index"])[0,...,1].astype(float)-zero)*scale
        cache[row["frame"]]=heat;return heat
    candidates=[evaluate(validation,inference,t) for t in np.arange(.02,.91,.02)]
    selected=max(candidates,key=lambda r:(r["f1"],r["recall"],-r["false_positives"]))
    measured=evaluate(test,inference,selected["threshold"],details=True)
    report=dict(scope="quantized SortScrews public test; not ESP32 turntable mission",
                model=str(a.model),split=a.split,frames=len(test),
                quantized_validation=candidates,selected=selected,**measured)
    a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({k:v for k,v in report.items() if k!="details"},indent=2))


if __name__ == "__main__":
    main()
