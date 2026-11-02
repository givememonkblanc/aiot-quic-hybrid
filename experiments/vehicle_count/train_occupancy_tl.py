#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path

import numpy as np

INPUT = 160
ROOT = Path(__file__).resolve().parents[2]


def load_split(root_dir: Path, split_file: Path):
    from PIL import Image

    xs = []
    ys = []
    for line in split_file.read_text().splitlines():
        rel, label = line.rsplit(" ", 1)
        img_path = root_dir / rel
        if not img_path.exists():
            continue
        img = Image.open(img_path).convert("L").resize((INPUT, INPUT), Image.Resampling.BILINEAR)
        xs.append(np.asarray(img, dtype=np.float32) / 255.0)
        ys.append(int(label))
    if not xs:
        raise SystemExit(f"no usable samples from {split_file}")
    return np.stack(xs)[..., None], np.asarray(ys, dtype=np.int64)


def build_model(alpha=0.35, freeze_backbone=True):
    import tensorflow as tf
    from keras import layers as L
    from keras.applications import MobileNetV2

    inp = L.Input((INPUT, INPUT, 1), name="input_luma")
    rgb = L.Concatenate(axis=-1)([inp, inp, inp])
    base = MobileNetV2(input_tensor=rgb, weights="imagenet", alpha=alpha, include_top=False)
    base.trainable = not freeze_backbone
    x = L.GlobalAveragePooling2D()(base.output)
    x = L.Dense(32, activation="relu", name="head_dense")(x)
    out = L.Dense(2, activation="softmax", name="head_logits")(x)
    return tf.keras.Model(inp, out), base


def main() -> int:
    import tensorflow as tf

    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path,
                    default=ROOT / "datasets/cnrpark/CNRPark-Patches-150x150")
    ap.add_argument("--train-split", type=Path,
                    default=ROOT / "datasets/cnrpark/splits/splits/CNRParkAB/train.txt")
    ap.add_argument("--val-split", type=Path,
                    default=ROOT / "datasets/cnrpark/splits/splits/CNRParkAB/val.txt")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).parent / "out_occupancy")
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--fine-tune-epochs", type=int, default=3)
    ap.add_argument("--lr", type=float, default=1e-3)
    args = ap.parse_args()

    x_tr, y_tr = load_split(args.root, args.train_split)
    x_va, y_va = load_split(args.root, args.val_split)
    ytr_oh = np.eye(2, dtype=np.float32)[y_tr]
    yva_oh = np.eye(2, dtype=np.float32)[y_va]
    print(f"dataset: {len(x_tr)} train / {len(x_va)} val")

    model, base = build_model(alpha=0.35, freeze_backbone=True)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(args.lr),
        loss="categorical_crossentropy",
        metrics=["accuracy"],
    )
    model.fit(x_tr, ytr_oh, validation_data=(x_va, yva_oh),
              epochs=args.epochs, batch_size=args.batch, verbose=2)

    if args.fine_tune_epochs > 0:
        for layer in base.layers[-20:]:
            layer.trainable = True
        model.compile(
            optimizer=tf.keras.optimizers.Adam(args.lr * 0.1),
            loss="categorical_crossentropy",
            metrics=["accuracy"],
        )
        model.fit(x_tr, ytr_oh, validation_data=(x_va, yva_oh),
                  epochs=args.fine_tune_epochs, batch_size=64, verbose=2)

    pred = model.predict(x_va, batch_size=64, verbose=0)[:, 1]
    for th in [0.5, 0.6, 0.7, 0.8, 0.9]:
        yp = pred > th
        tp = int(((yp == 1) & (y_va == 1)).sum())
        fp = int(((yp == 1) & (y_va == 0)).sum())
        fn = int(((yp == 0) & (y_va == 1)).sum())
        prec = tp / max(1, tp + fp)
        rec = tp / max(1, tp + fn)
        f1 = 0.0 if prec + rec == 0 else (2 * prec * rec / (prec + rec))
        acc = float((yp == y_va).mean())
        print(f"th={th:.2f} acc={acc:.3f} P={prec:.3f} R={rec:.3f} F1={f1:.3f}")

    args.out.mkdir(parents=True, exist_ok=True)
    model.save_weights(args.out / "occupancy.weights.h5")

    def rep_data():
        for i in range(0, min(len(x_tr), 300)):
            yield [x_tr[i:i + 1].astype(np.float32)]

    func = tf.function(lambda x: model(x))
    cf = func.get_concrete_function(tf.TensorSpec([1, INPUT, INPUT, 1], tf.float32))
    conv = tf.lite.TFLiteConverter.from_concrete_functions([cf])
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep_data
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()
    tfl_path = args.out / "occupancy_int8.tflite"
    tfl_path.write_bytes(tfl)
    print(f"int8 model: {len(tfl)} bytes -> {tfl_path}")

    c_path = args.out / "ai_inference_vehicle_model.c"
    with open(c_path, "w") as f:
        f.write("/* Parking occupancy classification model (int8), trained by\n"
                " * experiments/vehicle_count/train_occupancy_tl.py. */\n"
                "#include <stddef.h>\n\n"
                "const unsigned char g_vehicle_fomo_model_data[] = {\n")
        for i in range(0, len(tfl), 16):
            f.write("    " + ",".join(f"0x{b:02x}" for b in tfl[i:i+16]) + ",\n")
        f.write("};\n")
        f.write(f"const unsigned int g_vehicle_fomo_model_len = {len(tfl)}U;\n")
    (args.out / "asset_snippet.h").write_text(
        "/* ai_inference_model_asset_t values for the occupancy model */\n"
        f"/* .data = g_vehicle_fomo_model_data, .len = {len(tfl)}U */\n"
        "/* .input_width/height = 160, .output = [1,2] int8 softmax */\n"
        "/* .output_type = AI_INFERENCE_OUTPUT_CLASSIFICATION */\n"
        "/* .class_count = 2, .background_index = 0xFF, .target_class_index = 1 */\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
