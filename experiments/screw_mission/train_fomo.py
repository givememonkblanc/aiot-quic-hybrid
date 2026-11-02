#!/usr/bin/env python3
"""Train a one-class 96x96 int8 FOMO screw prototype with frozen splits."""
import argparse
import importlib.util
import json
import os
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
import cv2
import numpy as np
from PIL import Image
import torch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "fomo_recipe", ROOT / "experiments/vehicle_count/train_fomo_pt.py")
recipe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recipe)


def load(rows, split):
    xs, ys = [], []
    for row in rows:
        if row["split"] != split:
            continue
        image = Image.open(row["frame"]).convert("L").resize((96, 96), Image.Resampling.BILINEAR)
        grid = np.zeros((12, 12), np.float32)
        for obj in row["objects"]:
            grid[min(11, int(obj["cy"]*12)), min(11, int(obj["cx"]*12))] = 1
        xs.append(np.asarray(image, np.float32)/255.0)
        ys.append(grid)
    if not xs:
        raise ValueError(f"empty split: {split}")
    return np.stack(xs), np.stack(ys)


def score(predictions, truth, threshold):
    tp = fp = fn = 0
    for heat, target in zip(predictions, truth):
        _, _, stats, centers = cv2.connectedComponentsWithStats(
            (heat >= threshold).astype(np.uint8), 8)
        points = [(float(x)/12, float(y)/12) for x, y in centers[1:]]
        targets = [(float(x)/12, float(y)/12) for y, x in np.argwhere(target > 0)]
        pairs = sorted((np.hypot(px-tx, py-ty), i, j)
                       for i, (px, py) in enumerate(points)
                       for j, (tx, ty) in enumerate(targets))
        used_p, used_t = set(), set()
        for distance, i, j in pairs:
            if distance <= 2/12 and i not in used_p and j not in used_t:
                used_p.add(i); used_t.add(j)
        tp += len(used_t); fp += len(points)-len(used_p); fn += len(targets)-len(used_t)
    precision = tp/max(tp+fp, 1); recall = tp/max(tp+fn, 1)
    return dict(threshold=threshold, tp=tp, fp=fp, fn=fn,
                precision=precision, recall=recall,
                f1=2*precision*recall/max(precision+recall, 1e-9), missed=fn)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--labels", type=Path,
                   default=ROOT / "datasets/screw_training/public-labels.jsonl")
    p.add_argument("--out", type=Path,
                   default=ROOT / "experiments/screw_mission/out")
    p.add_argument("--epochs", type=int, default=50)
    p.add_argument("--batch", type=int, default=64)
    p.add_argument("--object-weight", type=float, default=120.0)
    p.add_argument("--seed", type=int, default=56)
    a = p.parse_args()
    rows = [json.loads(x) for x in a.labels.read_text().splitlines() if x.strip()]
    xtr, ytr = load(rows, "train"); xva, yva = load(rows, "val")
    xte, yte = load(rows, "public_test")
    torch.manual_seed(a.seed); np.random.seed(a.seed)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = recipe.FomoModel().to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=1e-3)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=a.epochs)
    rng = np.random.default_rng(a.seed)
    def tensor(x): return torch.from_numpy(x[:, None]).float().to(device)
    def target(y): return torch.from_numpy(recipe.to_onehot(y).transpose(0, 3, 1, 2)).float().to(device)
    xv, yv = tensor(xva), target(yva)
    for epoch in range(a.epochs):
        xb, yb = recipe.augment_batch(xtr, ytr, rng)
        order = rng.permutation(len(xb)); epoch_loss = 0.0
        model.train()
        for begin in range(0, len(order), a.batch):
            take = order[begin:begin+a.batch]
            optimizer.zero_grad(); pred = model(tensor(xb[take])); gt = target(yb[take])
            prob = pred[:, 1].clamp(1e-6, 1-1e-6); obj = gt[:, 1]
            ce = -(a.object_weight*obj*torch.log(prob) +
                   (1-obj)*torch.log(1-prob)).mean()
            tp = (prob*obj).sum(dim=(1, 2)); fp = (prob*(1-obj)).sum(dim=(1, 2)); fn = ((1-prob)*obj).sum(dim=(1, 2))
            dice = (1-(2*tp+1)/(2*tp+fp+fn+1)).mean()
            loss = ce + dice
            loss.backward(); optimizer.step(); epoch_loss += float(loss.detach())*len(take)
        scheduler.step(); epoch_loss /= len(order)
        if epoch == 0 or (epoch+1) % 10 == 0:
            model.eval()
            with torch.no_grad(): val_mean = float(model(xv)[:, 1].mean().cpu())
            print(f"epoch={epoch+1} loss={epoch_loss:.5f} val_positive_mean={val_mean:.5f}", flush=True)
    model.eval()
    with torch.no_grad():
        pv = model(tensor(xva))[:, 1].cpu().numpy()
        pt = model(tensor(xte))[:, 1].cpu().numpy()
    candidates = [score(pv, yva, t) for t in np.arange(.10, .91, .05)]
    selected = max(candidates, key=lambda r: (r["f1"], r["recall"], -r["fp"]))
    test = score(pt, yte, selected["threshold"])
    a.out.mkdir(parents=True, exist_ok=True)
    torch.save(model.state_dict(), a.out / "screw_fomo_weights.pt")
    recipe.export_tflite(model, xtr, a.out)
    (a.out / "fomo_vehicle_int8.tflite").rename(a.out / "screw_fomo_int8.tflite")
    report = dict(seed=a.seed, device=str(device), epochs=a.epochs,
                  batch=a.batch, object_weight=a.object_weight,
                  train_images=len(xtr), val_images=len(xva), public_test_images=len(xte),
                  validation=candidates, selected=selected, public_test=test,
                  scope="SortScrews public-domain prototype; not ESP32 turntable mission performance")
    (a.out / "public-validation.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(dict(selected=selected, public_test=test)), flush=True)


if __name__ == "__main__":
    main()
