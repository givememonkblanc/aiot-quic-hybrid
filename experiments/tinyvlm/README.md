# TinyVLM paper reconstruction

Source: [arXiv:2603.00136v1](https://arxiv.org/html/2603.00136v1), especially
§3, Algorithms 1, and Appendix A. This is an **independent implementation**,
not the author's unavailable checkpoint. It does not validate the paper's
accuracy, memory, or ESP32 performance claims. The current camera firmware
continues to use its existing model; no TinyVLM model has been flashed.

## Implemented

- MobileNetV2 width 0.35, RGB 128×128, GAP and 256-D linear embedding.
- Frozen CLIP ViT-B/32 image and caption features, four class prompt templates.
- Symmetric InfoNCE (temperature 0.07), shared student-to-teacher MSE projection,
  normalized prefixes 16/32/64/128/256 with equal-weight Matryoshka loss.
- AdamW, learning rate 1e-3, weight decay .01, warmup 10 epochs and cosine decay;
  defaults 100 epochs, 32 pairs per microbatch, 8 accumulated gradients. This
  increases effective update batch size, **not** the contrastive negative set.
- Fully INT8 TFLite export with a physically truncated final projection,
  calibration from up to 256 training images, class prototypes, C header,
  hashes and explicit unvalidated metadata.
- Host cosine classification; optional multi-scale grid proposals and classwise
  IoU NMS. This is a **separate reconstruction choice**, not the paper's detector.
- Opt-in ESP32 TFLM encoder and bounded C cosine matcher in
  `src/components/tinyvlm`. It consumes RGB128 images and returns embeddings;
  the matcher returns class indices/cosines, not calibrated probabilities.

## Explicit choices where the paper is underspecified

The paper specifies student text embeddings but not their full construction.
Here a learned bias-free 512→256 projection maps frozen CLIP text features into
student space. A shared 256→512 projection implements equation 6; its loss sums
squared errors over dimensions and averages over samples. Prefixes are
renormalized before cosine/InfoNCE. Prompt features are projected and truncated
before normalization, averaging and class normalization.

Equation 10 scales each class vector by its own absolute maximum, although the
text calls it per-channel quantization. We follow that **equation** (per-class
scalar). Positive scalar factors cancel in cosine; therefore MCU prototype
storage needs only K×d signed bytes. The image output zero point must still be
subtracted. Export retains scales for inspection in the host NPZ.

RGB is bilinearly resized to 128×128 and normalized to [-1,1]. Firmware callers
must provide identically prepared RGB; the existing grayscale FOMO preprocessor
is not compatible. This TensorFlow/Keras reconstruction starts the vision
backbone randomly; no CLIP transformer weights are copied into MobileNetV2.

The paper does not specify proposal generation or its detection head. `--regions`
uses the full image plus non-overlapping 2×2 and 3×3 crops (14 forwards), then
classwise NMS at IoU 0.5. Crop rectangles are proposal regions, not regressed
object boundaries. Similarity/margin defaults are **uncalibrated**. A one-class
candidate list cannot provide a rejection margin; include competing/background
classes and calibrate on held-out data. Neither this detector nor its latency is
claimed to reproduce the paper. Current classes are arbitrary named candidates,
not unrestricted on-device text generation.

## Environment

Locally checked with TensorFlow 2.17.1, NumPy, Pillow, PyTorch 2.5.1+ROCm and
Transformers (see the local validation report for exact versions). PyTorch is
used only for frozen CLIP cache generation. TensorFlow here trains on CPU.
`prepare.py` requires safetensors teacher weights; it does not bypass the
Transformers safety restriction on older PyTorch pickle loading. Dependencies
can be isolated from the firmware environment. No package upgrades are made by
these scripts.

## Train with real data

Create a JSONL manifest of image/caption pairs, with image paths relative to the
manifest, and a JSON list of candidate class names (example classes included):

```json
{"image":"images/001.jpg","caption":"a person standing beside a desk"}
{"image":"images/002.jpg","caption":"an empty room with a desk"}
```

These are schema examples, not an adequate dataset. Caption/image alignment
must be correct. Training caches are loaded into RAM; use a appropriately sized
local subset. Large CC3M-scale training needs a sharded streaming data pipeline,
which is not provided here. Do not call an in-domain training run zero-shot
without checking the training/evaluation class and image separation.

```bash
python3 experiments/tinyvlm/prepare.py \
  --manifest /data/train.jsonl \
  --classes experiments/tinyvlm/classes.example.json --out /data/teacher_cache.npz
python3 experiments/tinyvlm/train.py \
  --cache /data/teacher_cache.npz --out /data/tinyvlm-run
python3 experiments/tinyvlm/export.py \
  --run /data/tinyvlm-run --cache /data/teacher_cache.npz \
  --dim 64 --out /data/tinyvlm-bundle
python3 experiments/tinyvlm/infer.py \
  --bundle /data/tinyvlm-bundle --image /data/example.jpg
# Independent region-proposal baseline (14 forwards):
python3 experiments/tinyvlm/infer.py \
  --bundle /data/tinyvlm-bundle --image /data/example.jpg --regions
```

All run/export output paths must be new. `training.json` identifies the input
cache and marks accuracy unvalidated. An INT8 export is not evidence of usable
accuracy. `evaluate.py` accepts a separate `{"image":...,"label":...}` JSONL,
rejects byte-identical training images, and reports classification top-1 only:

```bash
python3 experiments/tinyvlm/evaluate.py --bundle /data/tinyvlm-bundle \
  --manifest /data/heldout.jsonl --training-cache /data/teacher_cache.npz \
  --out /data/heldout-report.json
```

Near duplicates and semantic/class leakage still need dataset review. Detection
AP and validation-based rejection-threshold selection are not implemented.

## ESP32 runtime

Enable `-D AIOT_TINYVLM_RUNTIME=ON` to make the component available; an application
using it adds `tinyvlm` to its `REQUIRES`. The runtime is opt-in and is not called
by the current FOMO application. With a trained exported bundle:

1. Include generated `tinyvlm_assets.h`, allocate a 16-byte-aligned tensor arena,
   and call `tinyvlm_encoder_init`. Arena requirement must be measured; the
   manifest deliberately leaves it null.
2. Supply a 128×128 RGB buffer matching host preprocessing to
   `tinyvlm_encoder_run`. It returns INT8 embedding, dimension and zero point.
3. Call `tinyvlm_cosine_i8` with the exported prototypes and returned zero point.
   Use class names from the manifest. Prefix dimensions may be reduced, never
   increased beyond the exported model/prototypes.
4. Implement application decisions only after held-out accuracy/rejection tests.
   Region detection requires explicit crops; the global classifier does not
   produce bounding boxes. The old FOMO output parser cannot consume embeddings.

There is one encoder context; callers must serialize init/invoke/reset. Model
and arena memory are caller-owned and must remain valid until reset. Init
verifies the FlatBuffer and shapes. Do not repartition/OTA based on the paper's
size estimate: the current OTA slot may be too small for the combined firmware.
No hardware speed or RAM measurement has yet been made for this runtime.

## Validation

```bash
TF_CPP_MIN_LOG_LEVEL=3 python3 -m unittest discover \
  -s experiments/tinyvlm -p 'test_*.py'
TF_CPP_MIN_LOG_LEVEL=2 OMP_NUM_THREADS=4 TF_NUM_INTRAOP_THREADS=4 \
  TF_NUM_INTEROP_THREADS=2 python3 experiments/tinyvlm/smoke.py \
  --out /tmp/tinyvlm-new-smoke-run
```

Smoke uses random features/images to verify gradients, INT8 conversion and
inference. Its checkpoints and exports are marked `smoke_only`; normal export
and inference reject them. This is not training or accuracy validation.

## Extended integration test (2026-09-21)

`TEST_REPORT.json` records a test using two generated color images and actual
CLIP teacher features. This checks implementation, not detection accuracy.
It trains three epochs, exports dimensions 16/64/256, compares float and INT8
embeddings, runs 14 crop proposals, checks smoke/output-overwrite guards, and
measures host single-thread TFLite invoke time (excludes preprocessing, teacher,
transport and display; not ESP32 latency).

```bash
TF_CPP_MIN_LOG_LEVEL=3 OMP_NUM_THREADS=4 TF_NUM_INTRAOP_THREADS=4 \
  TF_NUM_INTEROP_THREADS=2 python3 experiments/tinyvlm/validate_pipeline.py \
  --cache /path/to/test_teacher_cache.npz --out /tmp/tinyvlm-new-integration
```

Outputs always carry `smoke_only=true`. In addition, both ESP32 runtime source
files cross-compiled, and the generated C header/prototype self-match passed
ASan/UBSan on the host. This does not replace a linked firmware/hardware test.

## Real-image pilot (2026-09-21)

`PERSON_TRIAL_REPORT.json` records a single-seed COCO128 binary person-presence
trial: 78 training, 25 validation, 25 test images, exact-byte duplicates excluded.
ImageNet-initialized MobileNetV2 was adapted for 100 epochs with two label-derived
captions and actual CLIP teacher features. This is supervised in-domain adaptation,
not a zero-shot benchmark, localization evaluation, or official paper checkpoint.
The initial scores were validation 17/25 and test 19/25. Annotation audit found
one missing YOLO label file in each evaluation split (none in training); the
initial assumption that those images were backgrounds is not scored as ground
truth. Excluding them yields validation 16/24 (66.7%), test 19/24 (79.2%), person
recall 11/12, and four false positives among 12 backgrounds. Both provisional
and audited scores and excluded filenames are retained. The fixed CLIP prompt
baseline scored 13/24; this small prompt-specific comparison establishes no
general advantage over CLIP. No test-guided model selection was performed.

Artifacts are in ignored `person_trial/`; reproducible entry points are
`prepare_person_trial.py`, `prepare.py`, `train.py`, `export.py`, `evaluate.py`,
and `report_person_trial.py`. Training options were `--epochs 100 --batch-size 16
--accumulation 2 --backbone-weights imagenet`; export dimension was 64.

`esp32_benchmark/` is an isolated temporary firmware, with four held-out RGB
inputs prepared using `prepare_benchmark.py --trial experiments/tinyvlm/person_trial
--out experiments/tinyvlm/esp32_benchmark/main`. Build from the repository root:

```bash
source /home/ryzen395/esp-idf/export.sh
IDF_COMPONENT_MANAGER=0 idf.py -C experiments/tinyvlm/esp32_benchmark \
  -B experiments/tinyvlm/esp32_benchmark/build -D PROJECT_VER=tinyvlm-trial-v1 build
```

The benchmark reports quantization+encoder and prototype matching time, arena
usage, and free heap. It does not run camera or networking, and its rate must not
be presented as streaming FPS. It requests OTA rollback after the measurement;
safe deployment requires preserving the current release and confirming rollback
and recovery readiness. It has not been deployed. A read-only 8 MiB flash backup
is held in restricted, ignored `private/`; USB access/reset briefly restarted
the original firmware. Further board changes await user confirmation. The
isolated ESP-IDF 5.2.2 build linked successfully: 1,264,688 bytes, within the
2,031,616-byte OTA slot. Compilation is not an on-device measurement.
