"""One-to-one class-aware centroid matching in normalized image coordinates.
This evaluates FOMO point detections; it does not compute bounding-box mAP.
"""
import math


def match_points(gt, predictions, max_distance):
    if not 0 < max_distance <= math.sqrt(2):raise ValueError('invalid distance gate')
    for point in [*gt,*predictions]:
        if not (math.isfinite(point['x']) and math.isfinite(point['y'])
                and 0 <= point['x'] <= 1 and 0 <= point['y'] <= 1):
            raise ValueError('coordinates must be normalized to [0,1]')
    edges=[]
    for prediction in predictions:
        candidates=[]
        for index,target in enumerate(gt):
            if prediction['class_id'] != target['class_id']:continue
            distance=math.hypot(prediction['x']-target['x'],prediction['y']-target['y'])
            if distance <= max_distance:candidates.append((distance,index))
        edges.append([index for _,index in sorted(candidates)])
    owner={}
    def augment(pred,seen):
        for target in edges[pred]:
            if target in seen:continue
            seen.add(target)
            if target not in owner or augment(owner[target],seen):
                owner[target]=pred;return True
        return False
    for pred in range(len(predictions)):augment(pred,set())
    tp=len(owner);fp=len(predictions)-tp;fn=len(gt)-tp
    return dict(tp=tp,fp=fp,fn=fn,
        precision=tp/(tp+fp) if tp+fp else None,
        recall=tp/(tp+fn) if tp+fn else None,
        f1=2*tp/(2*tp+fp+fn) if 2*tp+fp+fn else None,
        matches=[dict(gt_index=g,prediction_index=p) for g,p in sorted(owner.items())])
