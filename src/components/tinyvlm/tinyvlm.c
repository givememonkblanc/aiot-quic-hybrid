#include "tinyvlm.h"
#include <math.h>

bool tinyvlm_quantize_rgb(const uint8_t *rgb, size_t bytes, float scale,
                         int zero_point, int8_t *output)
{
    if (!rgb || !output || !bytes || !isfinite(scale) || scale <= 0 ||
        zero_point < -128 || zero_point > 127) return false;
    for (size_t i=0; i<bytes; i++) {
        float real = (float)rgb[i] / 127.5f - 1.f;
        float value = roundf(real / scale) + zero_point;
        output[i] = value < -128 ? -128 : value > 127 ? 127 : (int8_t)value;
    }
    return true;
}

bool tinyvlm_cosine_i8(const int8_t *image, size_t image_dim, int image_zero_point,
                      const int8_t *prototypes, size_t classes, size_t stride,
                      size_t prefix_dim, float *scores, size_t *best_class)
{
    if (!image || !prototypes || !scores || !best_class || !classes || classes > 4096 ||
        !prefix_dim || prefix_dim > 256 || prefix_dim > image_dim || prefix_dim > stride ||
        stride > 256 || image_zero_point < -128 || image_zero_point > 127) return false;
    float norm = 0;
    for (size_t j=0; j<prefix_dim; j++) {
        float v = (int)image[j] - image_zero_point;
        norm += v*v;
    }
    if (norm <= 0) return false;
    float best = -2;
    *best_class = 0;
    for (size_t c=0; c<classes; c++) {
        float dot=0, cnorm=0;
        for (size_t j=0; j<prefix_dim; j++) {
            float v = prototypes[c*stride+j];
            dot += ((int)image[j]-image_zero_point)*v;
            cnorm += v*v;
        }
        if (cnorm <= 0) return false;
        scores[c] = fmaxf(-1, fminf(1, dot/sqrtf(norm*cnorm)));
        if (scores[c] > best) { best=scores[c]; *best_class=c; }
    }
    return true;
}
