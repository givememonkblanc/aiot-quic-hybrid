#!/usr/bin/env python3
"""Screw-centroid FOMO model for the turntable, from labelled sessions (label_turntable.py).

The device decodes a 1600x1200 frame at a quarter of its size, 400x300, and gives the model
the 160x120 region around the counting line (eval_turntable_count.ROI). The model sees luma
only; its output is a 20x15 grid of cells of 8x8 input pixels, with the probability that the
centre of a screw lies in a cell. Training uses that region and random 160x120 crops anywhere
on the plate, at offsets of whole cells so that the labels stay aligned. The sessions are
split whole: no frame of a validation or test session is seen in training.

The architecture and the int8 export follow experiments/vehicle_count/train_fomo_pt.py.

usage: train_turntable_fomo.py --labels L.jsonl --train S... --val S... --out DIR"""

import argparse
import importlib.util
import json
import os
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
import cv2
import numpy as np
import torch

from eval_turntable_count import ROI

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("fomo_recipe", ROOT / "experiments/vehicle_count/train_fomo_pt.py")
recipe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recipe)

# Two designs. "quarter": the frame decoded at 1/4 (400x300), a 160x120 region, output cells
# of 8 pixels. "eighth": the frame decoded at 1/8 (200x150) as the device does it quickly,
# an 80x60 region, output cells of 4 pixels: the same 20x15 grid over the same region.
# "fast": the eighth input, which goes down to the 20x15 grid in the first block. Its
# activations stay within the data cache of the device, whose tensor arena is in the slow
# PSRAM; "eighth" holds a 40x30x48 tensor and took 1.2 s for 8.4 million multiply-adds.
DESIGNS = {
    "quarter": dict(frame=(400, 300), cell=8, stem=24, head=24,
                    blocks=[(24, 32, 24, 1), (24, 48, 32, 2), (32, 48, 32, 1), (32, 64, 48, 2), (48, 96, 48, 1),
                            (48, 96, 48, 1)]),
    "eighth": dict(frame=(200, 150), cell=4, stem=16, head=16,
                   blocks=[(16, 32, 16, 1), (16, 48, 24, 2), (24, 72, 24, 1), (24, 72, 32, 1), (32, 96, 32, 1)]),
    "fast": dict(frame=(200, 150), cell=4, stem=8, head=16,
                 blocks=[(8, 24, 16, 2), (16, 48, 16, 1), (16, 48, 24, 1), (24, 72, 24, 1)]),
}
W = H = CELL = GW = GH = FW = FH = ROI_X = ROI_Y = None


def configure(name):
    global W, H, CELL, GW, GH, FW, FH, ROI_X, ROI_Y
    d = DESIGNS[name]
    FW, FH = d["frame"]
    CELL = d["cell"]
    ROI_X, ROI_Y = int(ROI[0] * FW), int(ROI[1] * FH)
    W, H = int(ROI[2] * FW) - ROI_X, int(ROI[3] * FH) - ROI_Y
    GW, GH = W // CELL, H // CELL
    return d


class Net(torch.nn.Module):
    """Stem, inverted-residual blocks (in, expand, out, stride) and a two-class head."""

    def __init__(self, d):
        super().__init__()
        nn = torch.nn
        self.stem_channels, self.block_cfg, self.head_channels = d["stem"], d["blocks"], d["head"]
        self.stem = nn.Conv2d(1, d["stem"], 3, stride=2, padding=1, bias=False)
        self.bn0 = nn.BatchNorm2d(d["stem"])
        self.blocks = nn.ModuleList(recipe.Bottleneck(*b) for b in d["blocks"])
        self.head1 = nn.Conv2d(d["blocks"][-1][2], d["head"], 1, bias=True)
        self.head2 = nn.Conv2d(d["head"], 2, 1, bias=True)

    def forward(self, x):
        F = torch.nn.functional
        x = F.relu6(self.bn0(self.stem(x)))
        for b in self.blocks:
            x = b(x)
        return F.softmax(self.head2(F.relu(self.head1(x))), dim=1)


def load(rows):
    """Frames at 400x300 luma and the screw centres in its pixels."""
    out = []
    for r in rows:
        gray = cv2.resize(cv2.imread(r["frame"], cv2.IMREAD_GRAYSCALE), (FW, FH), interpolation=cv2.INTER_AREA)
        pts = np.array([(o["cx"] * FW, o["cy"] * FH) for o in r["objects"]], np.float32).reshape(-1, 2)
        out.append((gray, pts))
    return out


def crop(gray, pts, x0, y0):
    img = gray[y0:y0 + H, x0:x0 + W].astype(np.float32) / 255.0
    grid = np.zeros((GH, GW), np.float32)
    for x, y in pts:
        gx, gy = int((x - x0) // CELL), int((y - y0) // CELL)
        if 0 <= gx < GW and 0 <= gy < GH:
            grid[gy, gx] = 1.0
    return img, grid



def sample(data, rng, count):
    xs, ys = [], []
    for _ in range(count):
        gray, pts = data[rng.integers(len(data))]
        if rng.random() < 0.5:
            x0, y0 = ROI_X + CELL * rng.integers(-2, 3), ROI_Y + CELL * rng.integers(-2, 3)
        else:
            x0, y0 = CELL * rng.integers(0, (FW - W) // CELL + 1), CELL * rng.integers(0, (FH - H) // CELL + 1)
        x0, y0 = int(np.clip(x0, 0, FW - W)), int(np.clip(y0, 0, FH - H))
        img, grid = crop(gray, pts, x0, y0)
        img = np.clip(img * rng.uniform(0.7, 1.3) + rng.uniform(-0.12, 0.12), 0, 1)
        if rng.random() < 0.5:
            img, grid = img[:, ::-1], grid[:, ::-1]
        xs.append(img)
        ys.append(grid)
    return np.stack(xs), np.stack(ys)


def roi_set(data):
    pairs = [crop(g, p, ROI_X, ROI_Y) for g, p in data]
    return np.stack([a for a, _ in pairs]), np.stack([b for _, b in pairs])


def gaussian(g, sigma):
    """Largest of the Gaussians around the labelled cells (B, H, W)."""
    r = 2
    k = torch.arange(-r, r + 1, device=g.device, dtype=torch.float32)
    kern = torch.exp(-(k[:, None] ** 2 + k[None, :] ** 2) / (2 * sigma ** 2))
    out = torch.nn.functional.max_pool2d(g[:, None], 1)  # copy
    pad = torch.nn.functional.pad(g[:, None], (r, r, r, r))
    unf = torch.nn.functional.unfold(pad, 2 * r + 1)          # B, 25, H*W
    vals = (unf * kern.flatten()[None, :, None]).max(1).values
    return vals.view_as(g)


def cell_scores(prob, truth, threshold):
    pred = prob >= threshold
    tp = int((pred & (truth > 0.5)).sum())
    return tp / max(1, int(pred.sum())), tp / max(1, int((truth > 0.5).sum()))


def export(model, rep, out):
    import tensorflow as tf
    from tensorflow.keras import layers as L

    def conv_w(c):
        w = [np.transpose(c.weight.detach().cpu().numpy(), (2, 3, 1, 0))]
        return w + ([c.bias.detach().cpu().numpy()] if c.bias is not None else [])

    def dw_w(c):
        return [np.transpose(c.weight.detach().cpu().numpy(), (2, 3, 0, 1))]

    def bn_w(b):
        return [t.detach().cpu().numpy() for t in (b.weight, b.bias, b.running_mean, b.running_var)]

    # A strided 3x3 layer pads one pixel on every side, as PyTorch does. Keras "same" pads an
    # even input only after it, which moves the output by one cell.
    def strided(layer, x):
        return layer(L.ZeroPadding2D(((1, 1), (1, 1)))(x))

    inp = L.Input((H, W, 1))
    stem = L.Conv2D(model.stem_channels, 3, strides=2, padding="valid", use_bias=False)
    bn0 = L.BatchNormalization()
    x = L.ReLU(6.0)(bn0(strided(stem, inp)))
    keras_blocks = []
    for inc, expand, outc, stride in model.block_cfg:
        residual = stride == 1 and inc == outc
        k = dict(conv1=L.Conv2D(expand, 1, use_bias=False), bn1=L.BatchNormalization(),
                 dw=L.DepthwiseConv2D(3, strides=stride, padding="same" if stride == 1 else "valid", use_bias=False),
                 bn2=L.BatchNormalization(),
                 conv2=L.Conv2D(outc, 1, use_bias=False), bn3=L.BatchNormalization(),
                 skip=None if residual else L.Conv2D(outc, 1, strides=stride, use_bias=False))
        y = L.ReLU(6.0)(k["bn1"](k["conv1"](x)))
        y = L.ReLU(6.0)(k["bn2"](k["dw"](y) if stride == 1 else strided(k["dw"], y)))
        y = k["bn3"](k["conv2"](y))
        x = L.ReLU(6.0)(L.Add()([x if residual else k["skip"](x), y]))
        keras_blocks.append(k)
    head1 = L.Conv2D(model.head_channels, 1, activation="relu")
    head2 = L.Conv2D(2, 1, activation="softmax")
    km = tf.keras.Model(inp, head2(head1(x)))
    stem.set_weights(conv_w(model.stem))
    bn0.set_weights(bn_w(model.bn0))
    for k, b in zip(keras_blocks, model.blocks):
        k["conv1"].set_weights(conv_w(b.conv1))
        k["bn1"].set_weights(bn_w(b.bn1))
        k["dw"].set_weights(dw_w(b.dw))
        k["bn2"].set_weights(bn_w(b.bn2))
        k["conv2"].set_weights(conv_w(b.conv2))
        k["bn3"].set_weights(bn_w(b.bn3))
        if b.skip is not None:
            k["skip"].set_weights(conv_w(b.skip))
    head1.set_weights(conv_w(model.head1))
    head2.set_weights(conv_w(model.head2))
    # the two implementations must agree before quantization
    with torch.no_grad():
        ref = model(torch.from_numpy(rep[:4, None]).float().to(next(model.parameters()).device)).cpu().numpy()
    got = km(rep[:4, ..., None]).numpy().transpose(0, 3, 1, 2)
    print(f"keras vs torch largest difference {np.abs(ref - got).max():.2e}")

    def rep_data():
        for i in range(min(len(rep), 300)):
            yield [rep[i:i + 1, ..., None].astype(np.float32)]

    func = tf.function(lambda x: km(x))
    conv = tf.lite.TFLiteConverter.from_concrete_functions(
        [func.get_concrete_function(tf.TensorSpec([1, H, W, 1], tf.float32))])
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep_data
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()
    (out / "screw_turntable_int8.tflite").write_bytes(tfl)
    with open(out / "ai_inference_vehicle_model.c", "w") as f:
        f.write("/* Turntable screw FOMO model (int8), experiments/screw_mission/train_turntable_fomo.py. */\n"
                "#include <stddef.h>\n\nconst unsigned char g_vehicle_fomo_model_data[] = {\n")
        for i in range(0, len(tfl), 16):
            f.write("    " + ",".join(f"0x{b:02x}" for b in tfl[i:i + 16]) + ",\n")
        f.write(f"}};\nconst unsigned int g_vehicle_fomo_model_len = {len(tfl)}U;\n")
    print(f"int8 model {len(tfl)} bytes")
    it = tf.lite.Interpreter(model_content=tfl)
    it.allocate_tensors()
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    worst = 0.0
    for k in range(min(8, len(rep))):
        q = np.clip(np.round(rep[k] / i["quantization"][0]) + i["quantization"][1], -128, 127).astype(np.int8)
        it.set_tensor(i["index"], q[None, :, :, None])
        it.invoke()
        got8 = (it.get_tensor(o["index"])[0, ..., 1].astype(np.float32) - o["quantization"][1]) * o["quantization"][0]
        with torch.no_grad():
            want = model(torch.from_numpy(rep[k:k + 1, None]).float().to(next(model.parameters()).device))[0, 1].cpu().numpy()
        worst = max(worst, float(np.abs(got8 - want).max()))
    print(f"int8 vs torch largest difference of the object probability {worst:.3f}")
    if worst > 0.2:
        raise SystemExit("the int8 model does not reproduce the trained one")
    return len(tfl)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--labels", type=Path, nargs="+", required=True)
    p.add_argument("--train", nargs="+", required=True, help="session names")
    p.add_argument("--val", nargs="+", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--design", choices=sorted(DESIGNS), default="eighth")
    p.add_argument("--steps", type=int, default=3000)
    p.add_argument("--batch", type=int, default=64)
    p.add_argument("--lr", type=float, default=2e-3)
    p.add_argument("--loss", choices=("dice", "focal", "heat"), default="heat",
                   help="focal: the penalty-reduced focal loss of CenterNet on a Gaussian around each centre, "
                        "so that close objects give separate maxima; heat: squared error to that Gaussian, "
                        "weighted towards the objects, which keeps the output below saturation: the int8 output "
                        "cannot tell apart two cells that are both near 1, and a plateau gives one maximum")
    p.add_argument("--sigma", type=float, default=0.7, help="Gaussian of the focal target, cells")
    a = p.parse_args()
    design = configure(a.design)
    rows = [json.loads(l) for path in a.labels for l in path.read_text().splitlines()]
    rows = [r for r in rows if not r["uncertain"]]
    train = load([r for r in rows if r["session"] in a.train])
    val = load([r for r in rows if r["session"] in a.val])
    print(f"frames: train {len(train)}, validation {len(val)}")
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    torch.manual_seed(7)
    model = Net(design).to(dev)
    print(f"design {a.design}: frame {FW}x{FH}, input {W}x{H}, grid {GW}x{GH}, "
          f"{sum(p.numel() for p in model.parameters())} parameters")
    xv, yv = roi_set(val)
    xvt = torch.from_numpy(xv[:, None]).float().to(dev)
    opt = torch.optim.Adam(model.parameters(), lr=a.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=a.steps)
    rng = np.random.default_rng(7)
    best = (-1.0, None)
    for step in range(1, a.steps + 1):
        xb, yb = sample(train, rng, a.batch)
        xb = torch.from_numpy(xb[:, None].copy()).float().to(dev)
        g = torch.from_numpy(yb.copy()).float().to(dev)
        model.train()
        pr = model(xb)[:, 1].clamp(1e-6, 1 - 1e-6)
        if a.loss == "dice":
            tp, fp, fn = (pr * g).sum((1, 2)), (pr * (1 - g)).sum((1, 2)), ((1 - pr) * g).sum((1, 2))
            loss = (1 - (2 * tp + 1) / (2 * tp + fp + fn + 1)).mean()
        elif a.loss == "heat":
            heat = gaussian(g, a.sigma)
            w = 1 + 20 * heat
            loss = (w * (pr - heat) ** 2).sum() / w.sum() * 100
        else:
            heat = gaussian(g, a.sigma)
            pos = g > 0.5
            lp = -((1 - pr) ** 2 * torch.log(pr))[pos].sum()
            ln = -((1 - heat) ** 4 * pr ** 2 * torch.log(1 - pr))[~pos].sum()
            loss = (lp + ln) / pos.sum().clamp_min(1)
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
        if step % 250 == 0 or step == a.steps:
            model.eval()
            with torch.no_grad():
                prob = model(xvt)[:, 1].cpu().numpy()
            pr_, rc_ = cell_scores(prob, yv, 0.5)
            f1 = 2 * pr_ * rc_ / max(1e-9, pr_ + rc_)
            print(f"step {step:5d} loss {loss.item():.3f}  validation cells P {pr_:.3f} R {rc_:.3f} F1 {f1:.3f}", flush=True)
            if f1 > best[0]:
                best = (f1, {k: v.detach().clone() for k, v in model.state_dict().items()})
    model.load_state_dict(best[1])
    model.eval()
    a.out.mkdir(parents=True, exist_ok=True)
    torch.save(model.state_dict(), a.out / "weights.pt")
    rep, _ = roi_set(train[::3])
    size = export(model, rep, a.out)
    (a.out / "training.json").write_text(json.dumps(dict(train=a.train, val=a.val, frames_train=len(train),
                                                         frames_val=len(val), best_val_cell_f1=best[0], steps=a.steps,
                                                         design=a.design, input=[W, H], roi=ROI, bytes=size), indent=1) + "\n")


if __name__ == "__main__":
    main()
