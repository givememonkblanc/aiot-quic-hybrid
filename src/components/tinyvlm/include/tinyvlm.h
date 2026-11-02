#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Prefix cosine similarity. Output scores are similarities, NOT probabilities.
 * Prototypes use symmetric per-class scalar quantization; positive scales
 * cancel in cosine similarity. image_scale cancels too, zero point does not. */
bool tinyvlm_cosine_i8(const int8_t *image, size_t image_dim, int image_zero_point,
                      const int8_t *prototypes, size_t classes, size_t stride,
                      size_t prefix_dim, float *scores, size_t *best_class);
bool tinyvlm_quantize_rgb(const uint8_t *rgb, size_t bytes, float scale,
                         int zero_point, int8_t *output);
/* Separate opt-in TFLM encoder. Caller owns model and 16-byte-aligned arena.
 * Returns false for an incompatible model. No default/untrained model exists. */
bool tinyvlm_encoder_init(const uint8_t *model, size_t model_bytes,
                         uint8_t *arena, size_t arena_bytes);
/* Exactly 128*128*3 RGB bytes; model input normalization is RGB / 127.5 - 1. */
bool tinyvlm_encoder_run(const uint8_t *rgb, size_t rgb_bytes,
                        int8_t *embedding, size_t capacity, size_t *dimensions,
                        int *zero_point);
void tinyvlm_encoder_reset(void);
size_t tinyvlm_encoder_arena_used_bytes(void);
#ifdef __cplusplus
}
#endif
