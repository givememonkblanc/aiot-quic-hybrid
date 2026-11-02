#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    unsigned width,height;
    size_t bytes,scratch_bytes;
    uint64_t decode_us,encode_us;
} moq_representation_metrics;
/* Caller owns output. No shared decoder scratch; RGB scratch is bounded/freed.
 * Synchronous profiling implementation; caller must measure QUIC pump stalls. */
bool moq_transcode(const uint8_t *jpeg,size_t length,unsigned level,
                   uint8_t *output,size_t capacity,moq_representation_metrics *metrics);
/* The frame at 1/8 (UXGA: 200x150, the raster of the local detector) at the given quality. */
/* A big-endian RGB565 raster as JPEG at the given quality (encode_us only). */
bool moq_encode_rgb565(const uint8_t *rgb,unsigned width,unsigned height,int quality,
                       uint8_t *output,size_t capacity,moq_representation_metrics *metrics);
bool moq_transcode_eighth(const uint8_t *jpeg,size_t length,int quality,
                          uint8_t *output,size_t capacity,moq_representation_metrics *metrics);
