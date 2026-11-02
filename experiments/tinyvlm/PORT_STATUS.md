# TinyVLM replacement audit — 2026-09-21 KST

Requested paper: Bibin Wilson, arXiv:2603.00136v1, https://arxiv.org/html/2603.00136v1 .

Status: an independent paper reconstruction is now implemented; see [README.md](README.md) and [VALIDATION.json](VALIDATION.json). Exact author-checkpoint replacement remains blocked by unavailable release artifacts. No TinyVLM weights have been flashed; existing person FOMO remains in use.

## Evidence

- The paper's reproducibility statement says code, pretrained models, quantized INT8 weights, class text embeddings and MCU examples will be released upon publication at an anonymous repository placeholder. No corresponding repository/checkpoint link is present in the paper.
- GitHub repository search for TinyVLM + Matryoshka found no matching repository. Hugging Face TinyVLM results were unrelated models; none was established as this paper's release. This is a bounded search, not proof that no private or unindexed release exists.
- Table 4's ESP32-S3 52 ms / 19 FPS is projected from specifications, not measured. Only MAX78000 is marked measured.
- Table 2 explicitly says TinyVLM numbers will be updated after full training. Published tables are not validated results for our ESP32.

## Compatibility work needed once artifacts exist

1. Verify author/release provenance, license, model and class-embedding hashes; obtain exact input preprocessing, dimensions, quantization and operators.
2. Current adapter interprets a FOMO spatial class grid (`populate_fomo_detections` in src/components/ai_inference/ai_inference_tflm.cc). The paper instead uses an embedding encoder, precomputed text prototypes and a detection head/region proposals. Swapping the byte array alone is invalid.
3. Need the actual detection head/proposal implementation, not only a global image classifier. Screw recognition/localization is unverified and needs a class prototype plus held-out object/background evaluation.
4. Check flash layout before deployment. Device boot detects 8 MiB physical flash, but current image/partition table uses 4 MiB with 0x1f0000 (2031616 byte) OTA app slots. Current instrumented binary is 1835472 bytes, with only 196144 bytes spare. The paper's 892 KB vision encoder cannot simply replace the current 57776-byte model within this slot; actual linked size needs a build. Detection configuration may be larger. No repartitioning has been performed.
5. Measure TFLM operator support, tensor arena, minimum internal heap, PSRAM, output sanity, complete inference timing and watchdog behavior locally before claiming MCU performance.
6. Preserve FOMO baseline data. Run TinyVLM comparisons separately with its own binary/model hashes. Do not merge different models into the active core/priority sweep.

Do not substitute an unrelated model named TinyVLM or untrained weights and call it the requested model. Recreating and distilling a new model from the paper is a separate training effort, not an exact pretrained-model swap.
