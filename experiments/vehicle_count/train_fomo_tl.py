#!/usr/bin/env python3
"""Train the FOMO vehicle-centroid model via TRANSFER LEARNING.

Root cause of the previous from-scratch model never learning: training a tiny
detector from scratch on small objects does not converge. Edge Impulse's FOMO
-- the exact architecture this firmware's contract mirrors -- starts from a
MobileNetV2 backbone PRE-TRAINED on ImageNet and only learns a small head on
top (transfer learning). The public Edge Impulse "Car Parking Occupancy
Detection - FOMO" project (96x96 grayscale, MobileNetV2 alpha 0.35) reaches
~84% validation accuracy this way.

This script reproduces that recipe:

    input  [1,96,96,1] int8 luma  (grayscale repeated to 3ch for the
                                   ImageNet RGB pretrained backbone)
    MobileNetV2 alpha=0.35 (imagenet weights), cut at block_6_expand_relu
                                   -> 12x12 spatial feature map
    head: Conv2D(32,1) -> Conv2D(2,1) -> softmax
    output [1,12,12,2] int8, channel 0 = background, channel 1 = vehicle

Because the base env's TensorFlow is CPU-only, we FREEZE the backbone and
PRECOMPUTE its features once, then train only the ~3.2K-param head on the
precomputed features. This is the classic transfer-learning fast path and needs
no GPU. Optionally unfreeze the final blocks and fine-tune (still CPU).

Run:
    python3 train_fomo_tl.py --labels datasets/pklot_fomo/vehicle_labels_sparse.jsonl --out out_tl

Outputs (--out):
    fomo_vehicle_int8.tflite int8 model for the firmware
    ai_inference_vehicle_model.c
    asset_snippet.h
    weights_head.h5
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

INPUT = 160
ROOT = Path(__file__).resolve().parents[2]


def load_dataset(frames_dir: Path, labels_path: Path, grid_size: int):
    """Load frames + centroid labels into [N,96,96] float and [N,12,12] grid."""
    from PIL import Image

    entries = [json.loads(l) for l in open(labels_path)]
    imgs, grids = [], []
    for e in entries:
        p = frames_dir / e["frame"]
        if not p.exists():
            continue
        img = Image.open(p).convert("L").resize((INPUT, INPUT), Image.Resampling.BILINEAR)
        grid = np.zeros((grid_size, grid_size), dtype=np.float32)
        for obj in e["objects"]:
            gx = min(int(obj["cx"] * grid_size), grid_size - 1)
            gy = min(int(obj["cy"] * grid_size), grid_size - 1)
            grid[gy, gx] = 1.0
        imgs.append(np.asarray(img, dtype=np.float32) / 255.0)
        grids.append(grid)
    if not imgs:
        raise SystemExit(f"no usable frames from {labels_path}")
    return np.stack(imgs), np.stack(grids)


def build_fomo(alpha=0.35, freeze_backbone=True, cut_layer="block_6_expand_relu"):
    """MobileNetV2(pretrained, frozen) cut at 1/8 + FOMO head. Input [96,96,1]."""
    import tensorflow as tf
    from keras import layers as L
    from keras.applications import MobileNetV2

    inp = L.Input((INPUT, INPUT, 1), name="input_luma")
    rgb = L.Concatenate(axis=-1, name="repeat_rgb")([inp, inp, inp])
    base = MobileNetV2(input_tensor=rgb, weights="imagenet",
                       alpha=alpha, include_top=True)
    base.trainable = not freeze_backbone
    cut = base.get_layer(cut_layer).output
    x = L.Conv2D(32, 1, activation="relu", name="fomo_head1")(cut)
    logits = L.Conv2D(2, 1, name="fomo_logits")(x)
    out = L.Softmax(name="fomo_out")(logits)
    return tf.keras.Model(inp, out), base


def soft_f1_loss(y_true, y_pred):
    """Soft F1 (Dice) loss on the object channel, recommended for FOMO
    car-park counting (DSORT-MCU, arXiv:2410.16769). Class-balanced by
    construction; pushes one prediction per object."""
    import tensorflow as tf

    p = tf.clip_by_value(y_pred[..., 1], 1e-6, 1.0)
    g = y_true[..., 1]
    tp = tf.reduce_sum(p * g, axis=[1, 2])
    fp = tf.reduce_sum(p * (1.0 - g), axis=[1, 2])
    fn = tf.reduce_sum((1.0 - p) * g, axis=[1, 2])
    return tf.reduce_mean(1.0 - (2.0 * tp + 1.0) / (2.0 * tp + fp + fn + 1.0))


def cell_metrics(y_grid, y_softmax):
    """y_grid: [N,12,12] binary; y_softmax: [N,12,12,2] predictions."""
    p = y_softmax[..., 1] > 0.5
    g = y_grid > 0.5
    tp = int(np.logical_and(p, g).sum())
    prec = tp / max(1, int(p.sum()))
    rec = tp / max(1, int(g.sum()))
    return prec, rec, tp, int(g.sum()), int(p.sum())


def param_count(weights):
    total = 0
    for w in weights:
        n = 1
        for dim in w.shape:
            n *= int(dim or 0)
        total += n
    return total


def main() -> int:
    import tensorflow as tf

    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=Path,
                    default=ROOT / "datasets/PKLot/PKLot/PKLot")
    ap.add_argument("--labels", type=Path,
                    default=ROOT / "datasets/pklot_fomo/vehicle_labels.jsonl")
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "out_tl")
    ap.add_argument("--epochs", type=int, default=80)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--obj-weight", type=float, default=100.0)
    ap.add_argument("--loss", choices=["xent", "softf1"], default="xent")
    ap.add_argument("--cut-layer", default="block_6_expand_relu")
    ap.add_argument("--fine-tune-epochs", type=int, default=0,
                    help="after head training, unfreeze last blocks and "
                         "fine-tune this many epochs (0 = skip)")
    args = ap.parse_args()

    model, base = build_fomo(alpha=0.35, freeze_backbone=True, cut_layer=args.cut_layer)
    grid_size = int(model.output_shape[1])

    x, y = load_dataset(args.frames, args.labels, grid_size)
    n_val = max(1, len(x) // 7)
    x_tr, y_tr, x_va, y_va = x[:-n_val], y[:-n_val], x[-n_val:], y[-n_val:]
    onehot = lambda g: np.stack([1.0 - g, g], axis=-1)
    ytr_oh, yva_oh = onehot(y_tr), onehot(y_va)
    print(f"dataset: {len(x_tr)} train / {len(x_va)} val, "
          f"{int(y.sum())} object cells total")

    total = param_count(model.weights)
    trainable = param_count(model.trainable_weights)
    print(f"model: {total} total params, {trainable} trainable (head only)")
    print(f"grid: {grid_size}x{grid_size} via {args.cut_layer}")

    prior = float(y_tr.mean())
    prior = min(max(prior, 1e-4), 1.0 - 1e-4)
    logits_bias = np.array([0.0, np.log(prior / (1.0 - prior))], dtype=np.float32)
    head1_w = model.get_layer("fomo_head1").get_weights()
    logits_w = model.get_layer("fomo_logits").get_weights()
    if len(head1_w) == 2:
        model.get_layer("fomo_head1").set_weights([head1_w[0], np.zeros_like(head1_w[1])])
    if len(logits_w) == 2:
        model.get_layer("fomo_logits").set_weights([logits_w[0], logits_bias])
    print(f"object-cell prior: {prior:.6f}")

    feat_model = tf.keras.Model(model.input, base.get_layer(args.cut_layer).output)
    print("precomputing frozen backbone features ...", flush=True)
    feat_tr = feat_model.predict(x_tr[..., None], batch_size=64, verbose=0)
    feat_va = feat_model.predict(x_va[..., None], batch_size=64, verbose=0)
    print(f"features: train {feat_tr.shape} val {feat_va.shape}")

    h1 = model.get_layer("fomo_head1")
    lg = model.get_layer("fomo_logits")
    sm = model.get_layer("fomo_out")
    head_in = tf.keras.Input(feat_tr.shape[1:])
    hd = tf.keras.Model(head_in, sm(lg(h1(head_in))))

    def weighted_xent(y_true, y_pred):
        p = tf.clip_by_value(y_pred, 1e-7, 1.0)
        w = tf.constant([1.0, args.obj_weight], dtype=tf.float32)
        return tf.reduce_mean(-tf.reduce_sum(y_true * tf.math.log(p) * w, axis=-1))

    loss_fn = weighted_xent if args.loss == "xent" else soft_f1_loss
    hd.compile(optimizer=tf.keras.optimizers.Adam(args.lr), loss=loss_fn)
    print("training head on frozen features ...", flush=True)
    hd.fit(feat_tr, ytr_oh, validation_data=(feat_va, yva_oh),
           epochs=args.epochs, batch_size=args.batch, verbose=2)

    pv = hd.predict(feat_va, batch_size=64, verbose=0)
    prec, rec, *_ = cell_metrics(y_va, pv)
    print(f"HEAD-ONLY cell P {prec:.2f} R {rec:.2f}")

    for layer_name in ["fomo_head1", "fomo_logits"]:
        model.get_layer(layer_name).set_weights(hd.get_layer(layer_name).get_weights())

    if args.fine_tune_epochs > 0:
        for l in base.layers:
            if l.name.startswith(("block_5", "block_6")):
                l.trainable = True
        model.compile(optimizer=tf.keras.optimizers.Adam(args.lr * 0.1),
                      loss=loss_fn)
        print("fine-tuning last blocks ...", flush=True)
        model.fit(x_tr[..., None], ytr_oh, validation_data=(x_va[..., None], yva_oh),
                  epochs=args.fine_tune_epochs, batch_size=64, verbose=2)

    pv_full = model.predict(x_va[..., None], batch_size=64, verbose=0)
    prec, rec, *_ = cell_metrics(y_va, pv_full)
    print(f"FULL MODEL cell P {prec:.2f} R {rec:.2f}")

    args.out.mkdir(parents=True, exist_ok=True)
    model.save_weights(args.out / "weights_head.weights.h5")

    export_tflite(model, x_tr, args.out)
    return 0


def export_tflite(model, x_tr, out: Path):
    """Export the Keras FOMO model to int8 TFLite matching the firmware contract."""
    import tensorflow as tf

    def rep_data():
        for i in range(0, min(len(x_tr), 300)):
            yield [x_tr[i:i + 1][..., None].astype(np.float32)]

    func = tf.function(lambda x: model(x))
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

    interp = tf.lite.Interpreter(model_content=tfl)
    inp = interp.get_input_details()[0]
    outp = interp.get_output_details()[0]
    print(f"tflite input:  {inp['shape']} {inp['dtype']}")
    print(f"tflite output: {outp['shape']} {outp['dtype']}")

    c_path = out / "ai_inference_vehicle_model.c"
    with open(c_path, "w") as f:
        f.write("/* FOMO vehicle-centroid model (int8), trained by\n"
                " * experiments/vehicle_count/train_fomo_tl.py via transfer\n"
                " * learning (MobileNetV2 alpha=0.35, ImageNet-pretrained). */\n"
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
