#pragma once

#include <stdint.h>

/*
 * Lightweight datagram object header, modeled on the MOQT object model
 * (draft-ietf-moq-transport). Prepended to every QUIC DATAGRAM payload so
 * the receiver can measure per-track loss, reordering, and age of
 * information. QUIC STREAM (MQTT) traffic is never framed with this header.
 */

#define AIOT_OBJECT_MAGIC_VER 0xA1U  /* magic 0xA (high nibble), version 1 */

#define AIOT_TRACK_TELEMETRY 1U
#define AIOT_TRACK_AI_RESULT 2U
#define AIOT_TRACK_IMAGE     3U

/* flags */
#define AIOT_OBJ_FLAG_EXPIRY_DROP_ALLOWED 0x01U

typedef struct __attribute__((packed)) {
    uint8_t  magic_ver;
    uint8_t  track_id;
    uint8_t  priority;    /* MOQT subscriber priority: lower is more urgent */
    uint8_t  flags;
    uint32_t object_seq;  /* monotonically increasing per track */
    uint32_t gen_ts_ms;   /* generation time, esp_timer domain */
} aiot_object_header_t;

#define AIOT_OBJECT_HEADER_SIZE ((size_t)sizeof(aiot_object_header_t))

_Static_assert(sizeof(aiot_object_header_t) == 12, "object header must stay 12 bytes");
