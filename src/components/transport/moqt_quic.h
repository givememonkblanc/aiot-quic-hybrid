#ifndef MOQT_QUIC_TRANSPORT_H
#define MOQT_QUIC_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MOQT_MAX_TOPIC_LEN 128
#define MOQT_MAX_PAYLOAD_SIZE 4096
#define MOQT_CONTROL_STREAM 0
#define MOQT_DATA_STREAM_START 1

typedef enum {
    MOQT_MSG_TYPE_SETUP = 0x00,
    MOQT_MSG_TYPE_SUBSCRIBE = 0x01,
    MOQT_MSG_TYPE_SUBSCRIBE_OK = 0x02,
    MOQT_MSG_TYPE_SUBSCRIBE_ERROR = 0x03,
    MOQT_MSG_TYPE_PUBLISH = 0x04,
    MOQT_MSG_TYPE_PUBLISH_OK = 0x05,
    MOQT_MSG_TYPE_PUBLISH_ERROR = 0x06,
    MOQT_MSG_TYPE_GOAWAY = 0x07,
} moqt_msg_type_t;

typedef struct {
    uint64_t track_id;
    uint64_t group_id;
    uint64_t object_id;
} moqt_location_t;

typedef struct {
    char topic[MOQT_MAX_TOPIC_LEN];
    uint8_t qos;
    uint8_t retain;
} moqt_subscribe_params_t;

typedef struct {
    moqt_location_t location;
    uint8_t flags;
} moqt_object_header_t;

typedef enum {
    MOQT_DELIVERY_SYNC = 0,
    MOQT_DELIVERY_ASYNC = 1,
} moqt_delivery_mode_t;

typedef struct {
    moqt_msg_type_t type;
    uint64_t request_id;
    union {
        struct {
            char version[32];
        } setup;
        struct {
            char topic[MOQT_MAX_TOPIC_LEN];
            uint8_t qos;
        } subscribe;
        struct {
            uint64_t request_id;
            uint8_t code;
        } subscribe_ok;
        struct {
            uint64_t request_id;
            uint8_t code;
            char reason[128];
        } error;
        struct {
            moqt_location_t location;
            uint8_t topic_len;
            char topic_data[MOQT_MAX_TOPIC_LEN];
        } publish;
        struct {
            moqt_location_t location;
        } publish_ok;
    } payload;
} moqt_message_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
} moqt_buffer_t;

void moqt_varint_encode(uint64_t value, uint8_t *out, size_t *out_len);
int moqt_varint_decode(const uint8_t *in, size_t in_len, uint64_t *out, size_t *consumed);

size_t moqt_encode_message(const moqt_message_t *msg, uint8_t *out, size_t max_len);
int moqt_decode_message(const uint8_t *in, size_t in_len, moqt_message_t *out);

void moqt_buffer_init(moqt_buffer_t *buf, uint8_t *data, size_t capacity);
int moqt_buffer_append(moqt_buffer_t *buf, const uint8_t *data, size_t len);

#endif