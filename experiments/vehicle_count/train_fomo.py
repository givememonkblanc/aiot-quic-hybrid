#!/usr/bin/env python3
"""Train a FOMO-style vehicle-centroid model and export it for the firmware.

Consumes frames (datasets/vehicle_raw) + YOLO auto-labels (autolabel.py) and
produces a full-int8 .tflite whose output matches the firmware's
populate_fomo_detections(): tensor [1, GRID, GRID, 2], channel 0 = background,
channel 1 = vehicle, int8 scores.

    python3 train_fomo.py                 # train + quantize + export C array
    python3 train_fomo.py --epochs 40

Outputs (experiments/vehicle_count/out/):
    fomo_vehicle_int8.tflite
    ai_inference_vehicle_model.c          # drop into src/components/ai_inference
    asset_snippet.h                       # values for the model asset struct
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

INPUT = 96
GRID = 12  # output stride 8
ROOT = Path(__file__).resolve().parents[2]


def load_dataset(frames_dir: Path, labels_path: Path):
    from PIL import Image

    entries = [json.loads(l) for l in open(labels_path)]
    imgs, grids = [], []
    for e in entries:
        p = frames_dir / e["frame"]
        if not p.exists():
            continue
        # Match firmware preprocessing: full-frame squash resize + luma.
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
    return np.stack(imgs)[..., None], np.stack(grids)


def augment_batch(x, y, rng):
    """Random shift (label-consistent), brightness and contrast jitter."""
    xb, yb = x.copy(), y.copy()
    for i in range(len(xb)):
        sx, sy = rng.integers(-8, 9, size=2)
        xb[i] = np.roll(xb[i], (sy, sx), axis=(0, 1))
        gy, gx = int(round(sy / (INPUT / GRID))), int(round(sx / (INPUT / GRID)))
        yb[i] = np.roll(yb[i], (gy, gx), axis=(0, 1))
        xb[i] = np.clip(xb[i] * rng.uniform(0.7, 1.3) + rng.uniform(-0.15, 0.15),
                        0.0, 1.0)
    return xb, yb


def build_model():
    import tensorflow as tf
    from tensorflow.keras import layers as L

    def dw_block(x, filters, stride):
        x = L.DepthwiseConv2D(3, strides=stride, padding="same", use_bias=False)(x)
        x = L.BatchNormalization()(x)
        x = L.ReLU(6.0)(x)
        x = L.Conv2D(filters, 1, use_bias=False)(x)
        x = L.BatchNormalization()(x)
        return L.ReLU(6.0)(x)

    inp = L.Input((INPUT, INPUT, 1))
    x = L.Conv2D(16, 3, strides=2, padding="same", use_bias=False)(inp)  # 48
    x = L.BatchNormalization()(x)
    x = L.ReLU(6.0)(x)
    x = dw_block(x, 32, 1)
    x = dw_block(x, 32, 2)   # 24
    x = dw_block(x, 48, 1)
    x = dw_block(x, 64, 2)   # 12
    x = dw_block(x, 64, 1)
    x = L.Conv2D(32, 1, activation="relu")(x)
    out = L.Conv2D(2, 1, activation="softmax")(x)  # {bg, vehicle}
    return tf.keras.Model(inp, out)


def to_onehot(y):
    return np.stack([1.0 - y, y], axis=-1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=Path, default=ROOT / "datasets/vehicle_raw")
    ap.add_argument("--labels", type=Path,
                    default=ROOT / "datasets/vehicle_labels.jsonl")
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "out")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--obj-weight", type=float, default=20.0)
    args = ap.parse_args()

    import tensorflow as tf

    x, y = load_dataset(args.frames, args.labels)
    n_val = max(1, len(x) // 7)  # last ~15% (chronological split)
    x_tr, y_tr, x_va, y_va = x[:-n_val], y[:-n_val], x[-n_val:], y[-n_val:]
    print(f"dataset: {len(x_tr)} train / {len(x_va)} val, "
          f"{int(y.sum())} object cells total")

    model = build_model()
    model.summary(line_length=90)

    w = np.array([1.0, args.obj_weight], dtype=np.float32)

    def loss_fn(yt, yp):
        ce = -yt * tf.math.log(tf.clip_by_value(yp, 1e-6, 1.0))
        return tf.reduce_mean(tf.reduce_sum(ce * w, axis=-1))

    model.compile(tf.keras.optimizers.Adam(1e-3), loss=loss_fn)

    rng = np.random.default_rng(7)
    yva1 = to_onehot(y_va)
    for epoch in range(args.epochs):
        xb, yb = augment_batch(x_tr, y_tr, rng)
        model.fit(xb, to_onehot(yb), batch_size=args.batch, epochs=1, verbose=0)
        val = model.evaluate(x_va, yva1, verbose=0)
        pred = model.predict(x_va, verbose=0)[..., 1] > 0.5
        gt = y_va > 0.5
        tp = int((pred & gt).sum())
        rec = tp / max(1, int(gt.sum()))
        prec = tp / max(1, int(pred.sum()))
        print(f"epoch {epoch+1:3d}  val_loss {val:.4f}  "
              f"cell P {prec:.2f} R {rec:.2f}")

    args.out.mkdir(parents=True, exist_ok=True)

    def rep_data():
        for i in range(0, min(len(x_tr), 300)):
            yield [x_tr[i : i + 1]]

    # Keras 3 + py3.12: from_keras_model and SavedModel export both hit
    # converter bugs in TF 2.17; a concrete function avoids both.
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
    tfl_path = args.out / "fomo_vehicle_int8.tflite"
    tfl_path.write_bytes(tfl)
    print(f"int8 model: {len(tfl)} bytes -> {tfl_path}")

    c_path = args.out / "ai_inference_vehicle_model.c"
    with open(c_path, "w") as f:
        f.write("/* FOMO vehicle-centroid model (int8), trained by\n"
                " * experiments/vehicle_count/train_fomo.py. */\n"
                "#include <stddef.h>\n\n"
                "const unsigned char g_vehicle_fomo_model_data[] = {\n")
        for i in range(0, len(tfl), 16):
            f.write("    " + ",".join(f"0x{b:02x}" for b in tfl[i:i+16]) + ",\n")
        f.write("};\n")
        f.write(f"const unsigned int g_vehicle_fomo_model_len = {len(tfl)}U;\n")
    (args.out / "asset_snippet.h").write_text(
        "/* ai_inference_model_asset_t values for the vehicle FOMO model */\n"
        f"/* .data = g_vehicle_fomo_model_data, .len = {len(tfl)}U */\n"
        "/* .input_width/height = 96, .output = [1,12,12,2] int8 */\n"
        "/* .output_type = AI_INFERENCE_OUTPUT_FOMO, .class_count = 1 */\n"
        "/* .background_index = 0, .detection_threshold = 128 */\n")
    print(f"C array: {c_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
