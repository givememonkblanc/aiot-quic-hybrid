#!/usr/bin/env python3
"""Screw FOMO model with a count head, trained for robustness to the scene (train_turntable_fomo.py,
design "fast", with the changes below; model-f1 stays reproducible with the old script).

Why (eval_scene_shift.py on model-f1): every missed screw is a screw touching another one (two
screws within one or two cells give one maximum of the 20x15 grid), and a new camera placement
brought detections off the plate (2 -> 242) and at the edge of the region. A screw alone is found
in every scene.

- Count head: per cell, background or 1, 2, 3 screws (softmax over 4). Screws whose cells are
  within MERGE cells of each other form one blob, labelled at their mean position with their
  number; the device emits a blob as that many detections at one place. Object probability = 1 -
  background, trained as before (squared error to a Gaussian around each blob, weighted towards it);
  the count by cross entropy at the blob cells. Same backbone: the cost on the device is unchanged.
- Input as on the device: the JPEG decoded at 1/8 by DCT scaling (PIL draft), RGB565 luma
  (replay_workers.Detector.luma), not a cv2 area resize of the full frame.
- Augmentation: scale 0.8-1.25, crops at any pixel offset over the whole frame (objects cut by the
  edge of the region, off-plate background), brightness and contrast, sensor noise, JPEG
  re-encoding, blur, horizontal flip; an unlabelled dark or grey mass entering from an edge of the
  region (occlude: the rim, a box or the background cut by the edge is not a screw).
- Sessions split whole; --test sessions are never seen in training nor used for selection.
- Selection on the validation sessions by screw-level F1 of the decoded output (peaks of the object
  probability over THRESHOLD, the count at each; matched to blobs within 1.5 cells).

usage: train_turntable_fomo2.py --labels L.jsonl... --train S... --val S... [--test S...] --out DIR"""

import argparse
import json
import os
import sys
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
import cv2
import numpy as np
import torch
from scipy.optimize import linear_sum_assignment

import train_turntable_fomo as T

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "moq"))
from replay_workers import Detector  # noqa: E402

CLASSES = 4          # background, 1, 2, 3 screws
MERGE = 1.5          # cells: screws closer than this are one blob
THRESHOLD = 0.3      # object probability of a detection, as model-f1 (77/255)
OCCLUDE = 0.0        # share of crops with an unlabelled mass entering from an edge (occlude)
NOISE = 0.04         # largest standard deviation of the sensor noise (luma 0..1)


class Net(T.Net):
    def __init__(self, d):
        super().__init__(d)
        self.head2 = torch.nn.Conv2d(d["head"], CLASSES, 1, bias=True)


def blobs(pts, cell):
    """screw centres (frame pixels) -> [(x, y, n)] with screws within MERGE cells joined."""
    n = len(pts)
    parent = list(range(n))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for i in range(n):
        for j in range(i + 1, n):
            if max(abs(pts[i][0] - pts[j][0]), abs(pts[i][1] - pts[j][1])) <= MERGE * cell:
                parent[find(i)] = find(j)
    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(pts[i])
    return [(float(np.mean([p[0] for p in g])), float(np.mean([p[1] for p in g])), len(g)) for g in groups.values()]


def load(rows):
    """Frames as the device sees them (200x150 luma) and the screw centres in its pixels."""
    out = []
    for r in rows:
        gray = Detector.luma(r["frame"]).astype(np.uint8)
        if gray.shape != (T.FH, T.FW):
            gray = cv2.resize(gray, (T.FW, T.FH), interpolation=cv2.INTER_AREA)
        pts = [(o["cx"] * T.FW, o["cy"] * T.FH) for o in r["objects"]]
        out.append((gray, pts))
    return out


def target(pts, x0, y0, s=1.0):
    """class map (GH, GW) for the region at (x0, y0) of the frame scaled by s."""
    grid = np.zeros((T.GH, T.GW), np.int64)
    for x, y, n in blobs([(px * s, py * s) for px, py in pts], T.CELL):
        gx, gy = int((x - x0) // T.CELL), int((y - y0) // T.CELL)
        if 0 <= gx < T.GW and 0 <= gy < T.GH:
            grid[gy, gx] = min(CLASSES - 1, grid[gy, gx] + n)
    return grid


def occlude(crop, rng):
    """An unlabelled dark or grey mass entering the region from one edge (a box, the rim, the
    background behind the plate), straight or slanted: the object cut by the edge of the region
    that is not a screw."""
    h, w = crop.shape
    level = int(rng.uniform(10, 120))
    depth = int(rng.integers(2, 3 * T.CELL))
    span = int(rng.integers(3 * T.CELL, max(3 * T.CELL + 1, w)))
    start = int(rng.integers(0, max(1, w - span)))
    slope = rng.uniform(-0.6, 0.6)
    side = int(rng.integers(4))
    yy, xx = np.mgrid[0:h, 0:w]
    if side == 0:     # top
        m = (yy < depth + slope * (xx - start)) & (xx >= start) & (xx < start + span)
    elif side == 1:   # bottom
        m = (h - 1 - yy < depth + slope * (xx - start)) & (xx >= start) & (xx < start + span)
    elif side == 2:   # left
        m = (xx < depth + slope * (yy - start)) & (yy >= start) & (yy < start + span)
    else:             # right
        m = (w - 1 - xx < depth + slope * (yy - start)) & (yy >= start) & (yy < start + span)
    crop[m] = np.clip(level + rng.normal(0, 8, int(m.sum())), 0, 255).astype(np.uint8)


def sample(data, rng, count):
    xs, ys = [], []
    for _ in range(count):
        gray, pts = data[rng.integers(len(data))]
        s = float(np.exp(rng.uniform(np.log(0.8), np.log(1.25))))
        img = cv2.resize(gray, (max(T.W, round(T.FW * s)), max(T.H, round(T.FH * s))), interpolation=cv2.INTER_LINEAR)
        hh, ww = img.shape
        if rng.random() < 0.5:   # around the counting region
            x0 = int(np.clip(T.ROI_X * s + rng.integers(-3 * T.CELL, 3 * T.CELL + 1), 0, ww - T.W))
            y0 = int(np.clip(T.ROI_Y * s + rng.integers(-3 * T.CELL, 3 * T.CELL + 1), 0, hh - T.H))
        else:                    # anywhere, the background off the plate included
            x0, y0 = int(rng.integers(0, ww - T.W + 1)), int(rng.integers(0, hh - T.H + 1))
        crop = img[y0:y0 + T.H, x0:x0 + T.W].copy()
        if rng.random() < OCCLUDE:
            occlude(crop, rng)
        if rng.random() < 0.3:
            crop = cv2.GaussianBlur(crop, (3, 3), rng.uniform(0.3, 1.0))
        if rng.random() < 0.5:
            q = int(rng.integers(15, 80))
            crop = cv2.imdecode(cv2.imencode(".jpg", crop, [cv2.IMWRITE_JPEG_QUALITY, q])[1], cv2.IMREAD_GRAYSCALE)
        x = crop.astype(np.float32) / 255.0
        x = x * rng.uniform(0.7, 1.3) + rng.uniform(-0.12, 0.12)
        x = x + rng.normal(0, rng.uniform(0, NOISE), x.shape)
        x = np.clip(x, 0, 1)
        g = target(pts, x0, y0, s)
        if rng.random() < 0.5:
            x, g = x[:, ::-1], g[:, ::-1]
        xs.append(x)
        ys.append(g)
    return np.stack(xs), np.stack(ys)


def roi_set(data):
    xs = np.stack([g[T.ROI_Y:T.ROI_Y + T.H, T.ROI_X:T.ROI_X + T.W].astype(np.float32) / 255.0 for g, _ in data])
    ys = [blobs(p, T.CELL) for _, p in data]
    return xs, ys


def decode(prob):
    """prob (CLASSES, GH, GW) -> [(gx, gy, n)]: 3x3 maxima of the object probability, count argmax."""
    obj = 1 - prob[0]
    out = []
    for y in range(T.GH):
        for x in range(T.GW):
            c = obj[y, x]
            if c < THRESHOLD:
                continue
            ys, xs = slice(max(0, y - 1), y + 2), slice(max(0, x - 1), x + 2)
            win = obj[ys, xs]
            if c < win.max():
                continue
            if any((yy * T.GW + xx) < (y * T.GW + x) and obj[yy, xx] == c
                   for yy in range(ys.start, min(T.GH, ys.stop)) for xx in range(xs.start, min(T.GW, xs.stop))):
                continue
            out.append((x, y, 1 + int(np.argmax(prob[1:, y, x]))))
    return out


def screw_scores(probs, truth):
    """screw-level hits, predicted and true screws over frames; truth: blobs in frame pixels."""
    hit = npred = ntrue = 0
    for prob, bl in zip(probs, truth):
        pred = decode(prob)
        tb = [((x - T.ROI_X) / T.CELL, (y - T.ROI_Y) / T.CELL, n) for x, y, n in bl
              if T.ROI_X <= x < T.ROI_X + T.W and T.ROI_Y <= y < T.ROI_Y + T.H]
        npred += sum(n for *_, n in pred)
        ntrue += sum(n for *_, n in tb)
        if not pred or not tb:
            continue
        C = np.array([[max(abs(px + 0.5 - tx), abs(py + 0.5 - ty)) for tx, ty, _ in tb] for px, py, _ in pred])
        r, c = linear_sum_assignment(np.where(C <= 1.5, C, 1e6))
        hit += sum(min(pred[i][2], tb[j][2]) for i, j in zip(r, c) if C[i, j] <= 1.5)
    return hit, npred, ntrue


def export(model, rep, out):
    """T.export with the CLASSES-channel head."""
    import tensorflow as tf
    real = tf.keras.layers.Conv2D

    def conv(filters, *a, **k):
        return real(CLASSES if filters == 2 and k.get("activation") == "softmax" else filters, *a, **k)
    tf.keras.layers.Conv2D = conv
    try:
        return T.export(model, rep, out)
    finally:
        tf.keras.layers.Conv2D = real


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--labels", type=Path, nargs="+", required=True)
    p.add_argument("--train", nargs="+", required=True)
    p.add_argument("--val", nargs="+", required=True)
    p.add_argument("--test", nargs="*", default=[])
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--steps", type=int, default=6000)
    p.add_argument("--batch", type=int, default=64)
    p.add_argument("--lr", type=float, default=2e-3)
    p.add_argument("--sigma", type=float, default=0.7)
    a = p.parse_args()
    design = T.configure("fast")
    rows = [json.loads(l) for path in a.labels for l in path.read_text().splitlines()]
    rows = [r for r in rows if not r.get("uncertain")]
    assert not set(a.test) & (set(a.train) | set(a.val)), "a test session is in training or validation"
    train = load([r for r in rows if r["session"] in a.train])
    val = load([r for r in rows if r["session"] in a.val])
    print(f"frames: train {len(train)}, validation {len(val)}", flush=True)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    torch.manual_seed(7)
    model = Net(design).to(dev)
    print(f"design fast+count: frame {T.FW}x{T.FH}, input {T.W}x{T.H}, grid {T.GW}x{T.GH}, "
          f"{sum(q.numel() for q in model.parameters())} parameters, {dev}", flush=True)
    xv, yv = roi_set(val)
    xvt = torch.from_numpy(xv[:, None]).float().to(dev)
    opt = torch.optim.Adam(model.parameters(), lr=a.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=a.steps)
    rng = np.random.default_rng(7)
    best = (-1.0, None)
    for step in range(1, a.steps + 1):
        xb, yb = sample(train, rng, a.batch)
        xb = torch.from_numpy(xb[:, None].copy()).float().to(dev)
        cls = torch.from_numpy(yb.copy()).to(dev)
        model.train()
        prob = model(xb).clamp(1e-6, 1 - 1e-6)
        obj = 1 - prob[:, 0]
        heat = T.gaussian((cls > 0).float(), a.sigma)
        w = 1 + 20 * heat
        loss = (w * (obj - heat) ** 2).sum() / w.sum() * 100
        pos = cls > 0
        if pos.any():
            pk = prob[:, 1:].permute(0, 2, 3, 1)[pos]
            loss = loss + torch.nn.functional.nll_loss(torch.log(pk / pk.sum(1, keepdim=True)), cls[pos] - 1)
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
        if step % 250 == 0 or step == a.steps:
            model.eval()
            with torch.no_grad():
                pv = model(xvt).cpu().numpy()
            hit, npred, ntrue = screw_scores(pv, yv)
            pr, rc = hit / max(npred, 1), hit / max(ntrue, 1)
            f1 = 2 * pr * rc / max(1e-9, pr + rc)
            print(f"step {step:5d} loss {loss.item():.3f}  validation screws P {pr:.3f} R {rc:.3f} F1 {f1:.3f}", flush=True)
            if f1 > best[0]:
                best = (f1, {k: v.detach().clone() for k, v in model.state_dict().items()})
    model.load_state_dict(best[1])
    model.eval()
    a.out.mkdir(parents=True, exist_ok=True)
    torch.save(model.state_dict(), a.out / "weights.pt")
    rep, _ = roi_set(train[::3])
    size = export(model, rep, a.out)
    (a.out / "training.json").write_text(json.dumps(dict(
        train=a.train, val=a.val, test=a.test, frames_train=len(train), frames_val=len(val),
        best_val_screw_f1=best[0], steps=a.steps, design="fast+count", classes=CLASSES, merge_cells=MERGE,
        input=[T.W, T.H], roi=T.ROI, bytes=size), indent=1) + "\n")


if __name__ == "__main__":
    main()
