#!/usr/bin/env python3
"""Train the FOMO vehicle-centroid model on the AMD GPU (ROCm) with PyTorch.

This is a PyTorch port of train_fomo.py. The base env's TensorFlow is CPU-only
and ROCm PyTorch kernels for the gfx1151 iGPU need HSA_OVERRIDE_GFX_VERSION
(RDNA4 -> gfx1100). Architecture, loss, and augmentation are identical to the
Keras original so the resulting model honours the firmware contract:

    input  [1,96,96,1] int8 luma (full-frame squash resize)
    output [1,12,12,2] int8, channel 0 = background, channel 1 = vehicle

Training runs on the GPU; the int8 .tflite export reuses the Keras converter
(the same code path as train_fomo.py) by loading the trained PyTorch weights
into an identical Keras model. Run with:

    HSA_OVERRIDE_GFX_VERSION=11.0.0 python3 train_fomo_pt.py --epochs 40

Outputs (--out):
    weights_pt.pt            PyTorch checkpoint (training only)
    fomo_vehicle_int8.tflite int8 model for the firmware
    ai_inference_vehicle_model.c
    asset_snippet.h
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

INPUT = 96
GRID = 12
ROOT = Path(__file__).resolve().parents[2]


class Bottleneck(nn.Module):
    """MobileNetV2 inverted residual: expand -> depthwise -> project + skip.

    The residual lets gradients and activations flow around the depthwise
    chain, which otherwise collapses the signal (the original FOMO block
    decayed activations ~10x per stage and never learned to detect).
    """

    def __init__(self, in_channels, expand, out_channels, stride):
        super().__init__()
        self.stride = stride
        self.use_res = stride == 1 and in_channels == out_channels
        self.conv1 = nn.Conv2d(in_channels, expand, 1, bias=False)
        self.bn1 = nn.BatchNorm2d(expand)
        self.dw = nn.Conv2d(expand, expand, 3, stride=stride, padding=1,
                            groups=expand, bias=False)
        self.bn2 = nn.BatchNorm2d(expand)
        self.conv2 = nn.Conv2d(expand, out_channels, 1, bias=False)
        self.bn3 = nn.BatchNorm2d(out_channels)
        self.skip = nn.Conv2d(in_channels, out_channels, 1, stride=stride,
                              bias=False) if not self.use_res else None

    def forward(self, x):
        y = F.relu6(self.bn1(self.conv1(x)))
        y = F.relu6(self.bn2(self.dw(y)))
        y = self.bn3(self.conv2(y))
        if self.use_res:
            return F.relu6(x + y)
        return F.relu6(self.skip(x) + y)


class FomoModel(nn.Module):
    def __init__(self):
        super().__init__()
        self.stem = nn.Conv2d(1, 24, 3, stride=2, padding=1, bias=False)
        self.bn0 = nn.BatchNorm2d(24)
        # (in, expand, out, stride)
        self.b1 = Bottleneck(24, 32, 24, 1)
        self.b2 = Bottleneck(24, 48, 32, 2)
        self.b3 = Bottleneck(32, 48, 32, 1)
        self.b4 = Bottleneck(32, 64, 48, 2)
        self.b5 = Bottleneck(48, 96, 48, 1)
        self.b6 = Bottleneck(48, 96, 48, 1)
        self.head1 = nn.Conv2d(48, 24, 1, bias=True)
        self.head2 = nn.Conv2d(24, 2, 1, bias=True)

    def forward(self, x):
        x = F.relu6(self.bn0(self.stem(x)))
        x = self.b1(x)
        x = self.b2(x)
        x = self.b3(x)
        x = self.b4(x)
        x = self.b5(x)
        x = self.b6(x)
        x = F.relu(self.head1(x))
        return F.softmax(self.head2(x), dim=1)


def load_dataset(frames_dir: Path, labels_path: Path):
    from PIL import Image

    entries = [json.loads(l) for l in open(labels_path)]
    imgs, grids = [], []
    for e in entries:
        p = frames_dir / e["frame"]
        if not p.exists():
            continue
        img = Image.open(p).convert("L").resize((INPUT, INPUT), Image.BILINEAR)
        grid = np.zeros((GRID, GRID), dtype=np.float32)
        for obj in e["objects"]:
            gx = min(int(obj["cx"] * GRID), GRID - 1)
            gy = min(int(obj["cy"] * GRID), GRID - 1)
            grid[gy, gx] = 1.0
        imgs.append(np.asarray(img, dtype=np.float32) / 255.0)
        grids.append(grid)
    if not imgs:
        raise SystemExit(f"no usable frames from {labels_path}")
    return np.stack(imgs), np.stack(grids)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=Path, default=ROOT / "datasets/PKLot/PKLot/PKLot")
    ap.add_argument("--labels", type=Path,
                    default=ROOT / "datasets/pklot_fomo/vehicle_labels.jsonl")
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "out")
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--obj-weight", type=float, default=100.0)
    ap.add_argument("--lr", type=float, default=1e-3)
    args = ap.parse_args()

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"device: {dev} "
          f"({torch.cuda.get_device_name(0)})" if dev == "cuda" else f"device: {dev}")

    x, y = load_dataset(args.frames, args.labels)
    n_val = max(1, len(x) // 7)
    x_tr, y_tr, x_va, y_va = x[:-n_val], y[:-n_val], x[-n_val:], y[-n_val:]
    print(f"dataset: {len(x_tr)} train / {len(x_va)} val, "
          f"{int(y.sum())} object cells total")

    model = FomoModel().to(dev)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"model params: {n_params}")

    def to_t(x):
        return torch.from_numpy(x).float()

    xva = to_t(x_va[:, None]).to(dev)
    yva = torch.from_numpy(to_onehot(y_va).transpose(0, 3, 1, 2)).to(dev)

    def loss_fn(pred, yb):
        # Soft F1 (Dice) loss on the object channel, as recommended for FOMO
        # car-park counting (DSORT-MCU, arXiv:2410.16769). Class-balanced by
        # construction: no manual object/background weight to tune, and it
        # pushes one prediction per object instead of firing everywhere.
        p = pred[:, 1].clamp_min(1e-6)
        g = yb[:, 1]
        tp = (p * g).sum(dim=(1, 2))
        fp = (p * (1.0 - g)).sum(dim=(1, 2))
        fn = ((1.0 - p) * g).sum(dim=(1, 2))
        return (1.0 - (2.0 * tp + 1.0) / (2.0 * tp + fp + fn + 1.0)).mean()

    rng = np.random.default_rng(7)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)
    for epoch in range(args.epochs):
        xb, yb = augment_batch(x_tr, y_tr, rng)
        xb = torch.from_numpy(xb[:, None]).float().to(dev)
        yb = torch.from_numpy(to_onehot(yb).transpose(0, 3, 1, 2)).float().to(dev)
        model.train()
        opt.zero_grad()
        pred = model(xb)
        loss = loss_fn(pred, yb)
        loss.backward()
        opt.step()
        sched.step()

        model.eval()
        with torch.no_grad():
            pv = model(xva)
            p = pv[:, 1].clamp_min(1e-6)
            g = yva[:, 1]
            tp = (p * g).sum()
            val_loss = (1.0 - (2.0 * tp + 1.0) /
                        (2.0 * tp + (p * (1.0 - g)).sum() +
                         ((1.0 - p) * g).sum() + 1.0)).item()
            pred_cell = pv[:, 1].cpu().numpy() > 0.5
            gt = y_va > 0.5
            tp = int((pred_cell & gt).sum())
            rec = tp / max(1, int(gt.sum()))
            prec = tp / max(1, int(pred_cell.sum()))
        print(f"epoch {epoch+1:3d}  val_loss {val_loss:.4f}  "
              f"cell P {prec:.2f} R {rec:.2f}", flush=True)

    args.out.mkdir(parents=True, exist_ok=True)
    torch.save(model.state_dict(), args.out / "weights_pt.pt")
    print(f"saved PyTorch weights: {args.out / 'weights_pt.pt'}")

    export_tflite(model, x_tr, args.out)
    return 0


def augment_batch(x, y, rng):
    xb, yb = x.copy(), y.copy()
    step = INPUT // GRID  # 8: grid cell size in input pixels
    for i in range(len(xb)):
        # Shift by whole grid cells so the label grid stays aligned with the
        # rolled image (a pixel shift that is not a multiple of `step` would
        # leave the labels up to half a cell out of registration).
        gc, gr = rng.integers(-1, 2, size=2)
        xb[i] = np.roll(xb[i], (gr * step, gc * step), axis=(0, 1))
        yb[i] = np.roll(yb[i], (gr, gc), axis=(0, 1))
        xb[i] = np.clip(xb[i] * rng.uniform(0.7, 1.3) + rng.uniform(-0.15, 0.15),
                        0.0, 1.0)
    return xb, yb


def to_onehot(y):
    return np.stack([1.0 - y, y], axis=-1)


def export_tflite(pt_model: FomoModel, x_tr, out: Path):
    """Build an identical Keras model, load PyTorch weights, export int8."""
    import tensorflow as tf
    from tensorflow.keras import layers as L

    def set_conv(klayer, pt_conv):
        w = pt_conv.weight.detach().cpu().numpy()
        weights = [np.transpose(w, (2, 3, 1, 0))]
        if pt_conv.bias is not None:
            weights.append(pt_conv.bias.detach().cpu().numpy())
        klayer.set_weights(weights)

    def set_dw(klayer, pt_conv):
        w = pt_conv.weight.detach().cpu().numpy()
        weights = [np.transpose(w, (2, 3, 0, 1))]
        if pt_conv.bias is not None:
            weights.append(pt_conv.bias.detach().cpu().numpy())
        klayer.set_weights(weights)

    def set_bn(klayer, pt_bn):
        g = pt_bn.weight.detach().cpu().numpy()
        b = pt_bn.bias.detach().cpu().numpy()
        m = pt_bn.running_mean.detach().cpu().numpy()
        v = pt_bn.running_var.detach().cpu().numpy()
        klayer.set_weights([g, b, m, v])

    class BottleneckKeras(L.Layer):
        def __init__(self, expand, out, stride, **kw):
            super().__init__(**kw)
            self.conv1 = L.Conv2D(expand, 1, use_bias=False)
            self.bn1 = L.BatchNormalization()
            self.dw = L.DepthwiseConv2D(3, strides=stride, padding="same",
                                        use_bias=False)
            self.bn2 = L.BatchNormalization()
            self.conv2 = L.Conv2D(out, 1, use_bias=False)
            self.bn3 = L.BatchNormalization()
            self.skip = L.Conv2D(out, 1, strides=stride,
                                 use_bias=False) if stride != 1 else None
            self.stride = stride

        def call(self, x):
            y = L.ReLU(6.0)(self.bn1(self.conv1(x)))
            y = L.ReLU(6.0)(self.bn2(self.dw(y)))
            y = self.bn3(self.conv2(y))
            if self.stride == 1:
                return L.ReLU(6.0)(x + y)
            return L.ReLU(6.0)(self.skip(x) + y)

    inp = L.Input((INPUT, INPUT, 1))
    x = L.Conv2D(24, 3, strides=2, padding="same", use_bias=False)(inp)
    x = L.BatchNormalization()(x)
    x = L.ReLU(6.0)(x)
    blocks_ker = [
        BottleneckKeras(32, 24, 1),
        BottleneckKeras(48, 32, 2),
        BottleneckKeras(48, 32, 1),
        BottleneckKeras(64, 48, 2),
        BottleneckKeras(96, 48, 1),
        BottleneckKeras(96, 48, 1),
    ]
    for b in blocks_ker:
        x = b(x)
    x = L.Conv2D(24, 1, activation="relu")(x)
    outl = L.Conv2D(2, 1, activation="softmax")(x)
    km = tf.keras.Model(inp, outl)

    set_conv(km.layers[1], pt_model.stem)
    set_bn(km.layers[2], pt_model.bn0)
    pt_blocks = [pt_model.b1, pt_model.b2, pt_model.b3,
                 pt_model.b4, pt_model.b5, pt_model.b6]
    for kb, pb in zip(blocks_ker, pt_blocks):
        set_conv(kb.conv1, pb.conv1)
        set_bn(kb.bn1, pb.bn1)
        set_dw(kb.dw, pb.dw)
        set_bn(kb.bn2, pb.bn2)
        set_conv(kb.conv2, pb.conv2)
        set_bn(kb.bn3, pb.bn3)
        if pb.skip is not None:
            set_conv(kb.skip, pb.skip)
    set_conv(km.layers[-2], pt_model.head1)
    set_conv(km.layers[-1], pt_model.head2)

    def rep_data():
        for i in range(0, min(len(x_tr), 300)):
            yield [x_tr[i:i + 1][..., None]]

    func = tf.function(lambda x: km(x))
    cf = func.get_concrete_function(
        tf.TensorSpec([1, INPUT, INPUT, 1], tf.float32))
    conv = tf.lite.TFLiteConverter.from_concrete_functions([cf])
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep_data
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()
    tfl_path = out / "fomo_vehicle_int8.tflite"
    tfl_path.write_bytes(tfl)
    print(f"int8 model: {len(tfl)} bytes -> {tfl_path}")

    c_path = out / "ai_inference_vehicle_model.c"
    with open(c_path, "w") as f:
        f.write("/* FOMO vehicle-centroid model (int8), trained by\n"
                " * experiments/vehicle_count/train_fomo_pt.py on the AMD GPU. */\n"
                "#include <stddef.h>\n\n"
                "const unsigned char g_vehicle_fomo_model_data[] = {\n")
        for i in range(0, len(tfl), 16):
            f.write("    " + ",".join(f"0x{b:02x}" for b in tfl[i:i+16]) + ",\n")
        f.write("};\n")
        f.write(f"const unsigned int g_vehicle_fomo_model_len = {len(tfl)}U;\n")
    (out / "asset_snippet.h").write_text(
        "/* ai_inference_model_asset_t values for the vehicle FOMO model */\n"
        f"/* .data = g_vehicle_fomo_model_data, .len = {len(tfl)}U */\n"
        "/* .input_width/height = 96, .output = [1,12,12,2] int8 */\n"
        "/* .output_type = AI_INFERENCE_OUTPUT_FOMO, .class_count = 1 */\n"
        "/* .background_index = 0, .detection_threshold = 128 */\n")
    print(f"C array: {c_path}")
    print(f"asset snippet: {out / 'asset_snippet.h'}")


if __name__ == "__main__":
    sys.exit(main())
