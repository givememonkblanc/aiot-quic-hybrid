#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AIOT_IMAGE_CHUNK_MAGIC 0xA5U
#define AIOT_IMAGE_CHUNK_VERSION 0x01U
#define AIOT_IMAGE_CHUNK_HEADER_SIZE 12U

typedef struct {
    uint16_t frame_id;
    uint16_t chunk_index;
    uint16_t chunk_count;
    uint16_t total_len;
    uint16_t payload_stride;
} aiot_image_chunk_header_t;

static inline void aiot_image_chunk_write_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static inline uint16_t aiot_image_chunk_read_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static inline bool aiot_image_chunk_parse(const uint8_t *payload,
    uint16_t payload_len,
    aiot_image_chunk_header_t *header,
    const uint8_t **chunk_payload,
    uint16_t *chunk_payload_len)
{
    if (payload == NULL ||
        payload_len <= AIOT_IMAGE_CHUNK_HEADER_SIZE ||
        payload[0] != AIOT_IMAGE_CHUNK_MAGIC ||
        payload[1] != AIOT_IMAGE_CHUNK_VERSION) {
        return false;
    }

    header->frame_id = aiot_image_chunk_read_u16(payload + 2);
    header->chunk_index = aiot_image_chunk_read_u16(payload + 4);
    header->chunk_count = aiot_image_chunk_read_u16(payload + 6);
    header->total_len = aiot_image_chunk_read_u16(payload + 8);
    header->payload_stride = aiot_image_chunk_read_u16(payload + 10);

    if (header->chunk_count == 0U ||
        header->chunk_index >= header->chunk_count ||
        header->total_len == 0U ||
        header->payload_stride == 0U) {
        return false;
    }

    *chunk_payload = payload + AIOT_IMAGE_CHUNK_HEADER_SIZE;
    *chunk_payload_len = (uint16_t)(payload_len - AIOT_IMAGE_CHUNK_HEADER_SIZE);
    return *chunk_payload_len > 0U;
}

static inline uint16_t aiot_image_chunk_encode(uint8_t *dst,
    uint16_t dst_capacity,
    uint16_t frame_id,
    uint16_t chunk_index,
    uint16_t chunk_count,
    uint16_t total_len,
    uint16_t payload_stride,
    const uint8_t *chunk_payload,
    uint16_t chunk_payload_len)
{
    if (dst == NULL ||
        chunk_payload == NULL ||
        dst_capacity < (uint16_t)(AIOT_IMAGE_CHUNK_HEADER_SIZE + chunk_payload_len) ||
        chunk_count == 0U ||
        chunk_index >= chunk_count ||
        total_len == 0U ||
        payload_stride == 0U ||
        chunk_payload_len > payload_stride ||
        chunk_payload_len == 0U) {
        return 0U;
    }

    dst[0] = AIOT_IMAGE_CHUNK_MAGIC;
    dst[1] = AIOT_IMAGE_CHUNK_VERSION;
    aiot_image_chunk_write_u16(dst + 2, frame_id);
    aiot_image_chunk_write_u16(dst + 4, chunk_index);
    aiot_image_chunk_write_u16(dst + 6, chunk_count);
    aiot_image_chunk_write_u16(dst + 8, total_len);
    aiot_image_chunk_write_u16(dst + 10, payload_stride);

    for (uint16_t i = 0; i < chunk_payload_len; ++i) {
        dst[AIOT_IMAGE_CHUNK_HEADER_SIZE + i] = chunk_payload[i];
    }

    return (uint16_t)(AIOT_IMAGE_CHUNK_HEADER_SIZE + chunk_payload_len);
}
