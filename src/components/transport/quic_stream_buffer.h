#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* Offsets increase for a single stream. Bytes stay immutable until ACKed. */
typedef struct {
    uint8_t *data;
    size_t capacity;
    uint64_t queued, submitted, acknowledged;
} quic_stream_buffer_t;

static inline bool quic_stream_buffer_push(quic_stream_buffer_t *b, const uint8_t *data, size_t len)
{
    if (len > b->capacity || b->queued - b->acknowledged > b->capacity - len) return false;
    size_t at = b->queued % b->capacity;
    size_t first = len < b->capacity - at ? len : b->capacity - at;
    memcpy(b->data + at, data, first);
    memcpy(b->data, data + first, len - first);
    b->queued += len;
    return true;
}

static inline size_t quic_stream_buffer_pending(quic_stream_buffer_t *b, uint8_t **data)
{
    size_t at = b->submitted % b->capacity;
    uint64_t left = b->queued - b->submitted;
    *data = b->data + at;
    return left < b->capacity - at ? (size_t)left : b->capacity - at;
}

static inline bool quic_stream_buffer_ack(quic_stream_buffer_t *b, uint64_t offset, uint64_t len)
{
    if (offset != b->acknowledged || len > b->submitted - b->acknowledged) return false;
    b->acknowledged += len;
    return true;
}
