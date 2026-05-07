#include "moqt_quic.h"
#include <string.h>

void moqt_varint_encode(uint64_t value, uint8_t *out, size_t *out_len)
{
    if (value <= 127) {
        out[0] = (uint8_t)value;
        *out_len = 1;
    } else if (value <= 16383) {
        out[0] = (uint8_t)((value >> 8) | 0x40);
        out[1] = (uint8_t)(value & 0xFF);
        *out_len = 2;
    } else if (value <= 1073741823) {
        out[0] = (uint8_t)((value >> 24) | 0x80);
        out[1] = (uint8_t)((value >> 16) & 0xFF);
        out[2] = (uint8_t)((value >> 8) & 0xFF);
        out[3] = (uint8_t)(value & 0xFF);
        *out_len = 4;
    } else if (value <= 4398046511103UL) {
        out[0] = (uint8_t)((value >> 56) | 0xC0);
        out[1] = (uint8_t)((value >> 48) & 0xFF);
        out[2] = (uint8_t)((value >> 40) & 0xFF);
        out[3] = (uint8_t)((value >> 32) & 0xFF);
        out[4] = (uint8_t)((value >> 24) & 0xFF);
        out[5] = (uint8_t)((value >> 16) & 0xFF);
        out[6] = (uint8_t)((value >> 8) & 0xFF);
        out[7] = (uint8_t)(value & 0xFF);
        *out_len = 8;
    } else {
        out[0] = 0xFE;
        out[1] = (uint8_t)((value >> 48) & 0xFF);
        out[2] = (uint8_t)((value >> 40) & 0xFF);
        out[3] = (uint8_t)((value >> 32) & 0xFF);
        out[4] = (uint8_t)((value >> 24) & 0xFF);
        out[5] = (uint8_t)((value >> 16) & 0xFF);
        out[6] = (uint8_t)((value >> 8) & 0xFF);
        out[7] = (uint8_t)(value & 0xFF);
        out[8] = (uint8_t)((value >> 56) & 0xFF);
        *out_len = 9;
    }
}

int moqt_varint_decode(const uint8_t *in, size_t in_len, uint64_t *out, size_t *consumed)
{
    if (in_len < 1) return -1;
    
    uint8_t lead = in[0];
    
    if ((lead & 0x80) == 0) {
        *out = lead;
        *consumed = 1;
    } else if ((lead & 0xC0) == 0x40) {
        if (in_len < 2) return -1;
        *out = ((lead & 0x3F) << 8) | in[1];
        *consumed = 2;
    } else if ((lead & 0xE0) == 0x80) {
        if (in_len < 4) return -1;
        *out = (((uint64_t)lead & 0x1F) << 24) | (((uint64_t)in[1]) << 16) |
               (((uint64_t)in[2]) << 8) | in[3];
        *consumed = 4;
    } else if ((lead & 0xF0) == 0xC0) {
        if (in_len < 8) return -1;
        *out = (((uint64_t)lead & 0x0F) << 56) | (((uint64_t)in[1]) << 48) |
               (((uint64_t)in[2]) << 40) | (((uint64_t)in[3]) << 32) |
               (((uint64_t)in[4]) << 24) | (((uint64_t)in[5]) << 16) |
               (((uint64_t)in[6]) << 8) | in[7];
        *consumed = 8;
    } else if (lead == 0xFE) {
        if (in_len < 9) return -1;
        *out = (((uint64_t)in[8]) << 56) | (((uint64_t)in[7]) << 48) |
               (((uint64_t)in[6]) << 40) | (((uint64_t)in[5]) << 32) |
               (((uint64_t)in[4]) << 24) | (((uint64_t)in[3]) << 16) |
               (((uint64_t)in[2]) << 8) | in[1];
        *consumed = 9;
    } else {
        return -1;
    }
    return 0;
}

size_t moqt_encode_message(const moqt_message_t *msg, uint8_t *out, size_t max_len)
{
    size_t pos = 0;
    uint8_t varint_buf[16];
    size_t varint_len;
    
    if (pos + 1 > max_len) return 0;
    out[pos++] = msg->type;
    
    moqt_varint_encode(msg->request_id, varint_buf, &varint_len);
    if (pos + varint_len > max_len) return 0;
    memcpy(&out[pos], varint_buf, varint_len);
    pos += varint_len;
    
    switch (msg->type) {
        case MOQT_MSG_TYPE_SETUP: {
            size_t ver_len = strlen(msg->payload.setup.version);
            if (pos + ver_len > max_len) return 0;
            memcpy(&out[pos], msg->payload.setup.version, ver_len);
            pos += ver_len;
            break;
        }
        case MOQT_MSG_TYPE_SUBSCRIBE: {
            size_t topic_len = strlen(msg->payload.subscribe.topic);
            moqt_varint_encode(topic_len, varint_buf, &varint_len);
            if (pos + varint_len + topic_len > max_len) return 0;
            memcpy(&out[pos], varint_buf, varint_len);
            pos += varint_len;
            memcpy(&out[pos], msg->payload.subscribe.topic, topic_len);
            pos += topic_len;
            if (pos + 1 > max_len) return 0;
            out[pos++] = msg->payload.subscribe.qos;
            break;
        }
        case MOQT_MSG_TYPE_SUBSCRIBE_OK: {
            if (pos + 1 > max_len) return 0;
            out[pos++] = msg->payload.subscribe_ok.code;
            break;
        }
        case MOQT_MSG_TYPE_SUBSCRIBE_ERROR: {
            if (pos + 1 > max_len) return 0;
            out[pos++] = msg->payload.error.code;
            size_t reason_len = strlen(msg->payload.error.reason);
            moqt_varint_encode(reason_len, varint_buf, &varint_len);
            if (pos + varint_len + reason_len > max_len) return 0;
            memcpy(&out[pos], varint_buf, varint_len);
            pos += varint_len;
            memcpy(&out[pos], msg->payload.error.reason, reason_len);
            pos += reason_len;
            break;
        }
        case MOQT_MSG_TYPE_PUBLISH: {
            moqt_varint_encode(msg->payload.publish.topic_len, varint_buf, &varint_len);
            if (pos + varint_len > max_len) return 0;
            memcpy(&out[pos], varint_buf, varint_len);
            pos += varint_len;
            if (pos + msg->payload.publish.topic_len > max_len) return 0;
            memcpy(&out[pos], msg->payload.publish.topic_data, msg->payload.publish.topic_len);
            pos += msg->payload.publish.topic_len;
            moqt_varint_encode(msg->payload.publish.location.group_id, varint_buf, &varint_len);
            if (pos + varint_len > max_len) return 0;
            memcpy(&out[pos], varint_buf, varint_len);
            pos += varint_len;
            moqt_varint_encode(msg->payload.publish.location.object_id, varint_buf, &varint_len);
            if (pos + varint_len > max_len) return 0;
            memcpy(&out[pos], varint_buf, varint_len);
            pos += varint_len;
            break;
        }
        case MOQT_MSG_TYPE_PUBLISH_OK: {
            moqt_varint_encode(msg->payload.publish_ok.location.group_id, varint_buf, &varint_len);
            if (pos + varint_len > max_len) return 0;
            memcpy(&out[pos], varint_buf, varint_len);
            pos += varint_len;
            moqt_varint_encode(msg->payload.publish_ok.location.object_id, varint_buf, &varint_len);
            if (pos + varint_len > max_len) return 0;
            memcpy(&out[pos], varint_buf, varint_len);
            pos += varint_len;
            break;
        }
        default:
            break;
    }
    
    return pos;
}

int moqt_decode_message(const uint8_t *in, size_t in_len, moqt_message_t *out)
{
    size_t pos = 0;
    size_t consumed;
    uint64_t val;
    
    if (in_len < 1) return -1;
    out->type = in[pos++];
    
    if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
    out->request_id = val;
    pos += consumed;
    
    switch (out->type) {
        case MOQT_MSG_TYPE_SETUP:
            break;
        case MOQT_MSG_TYPE_SUBSCRIBE:
            if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
            size_t topic_len = (size_t)val;
            pos += consumed;
            if (pos + topic_len > in_len) return -1;
            if (topic_len >= MOQT_MAX_TOPIC_LEN) topic_len = MOQT_MAX_TOPIC_LEN - 1;
            memcpy(out->payload.subscribe.topic, &in[pos], topic_len);
            out->payload.subscribe.topic[topic_len] = 0;
            pos += topic_len;
            if (pos < in_len) {
                out->payload.subscribe.qos = in[pos++];
            }
            break;
        case MOQT_MSG_TYPE_SUBSCRIBE_OK:
            if (pos < in_len) {
                out->payload.subscribe_ok.code = in[pos++];
            }
            break;
        case MOQT_MSG_TYPE_SUBSCRIBE_ERROR:
            if (pos < in_len) {
                out->payload.error.code = in[pos++];
            }
            if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
            size_t reason_len = (size_t)val;
            pos += consumed;
            if (pos + reason_len > in_len) return -1;
            if (reason_len >= 128) reason_len = 127;
            memcpy(out->payload.error.reason, &in[pos], reason_len);
            out->payload.error.reason[reason_len] = 0;
            pos += reason_len;
            break;
        case MOQT_MSG_TYPE_PUBLISH:
            if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
            out->payload.publish.topic_len = (size_t)val;
            pos += consumed;
            if (pos + out->payload.publish.topic_len > in_len) return -1;
            if (out->payload.publish.topic_len >= MOQT_MAX_TOPIC_LEN) {
                out->payload.publish.topic_len = MOQT_MAX_TOPIC_LEN - 1;
            }
            memcpy(out->payload.publish.topic_data, &in[pos], out->payload.publish.topic_len);
            out->payload.publish.topic_data[out->payload.publish.topic_len] = 0;
            pos += out->payload.publish.topic_len;
            if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
            out->payload.publish.location.group_id = val;
            pos += consumed;
            if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
            out->payload.publish.location.object_id = val;
            pos += consumed;
            break;
        case MOQT_MSG_TYPE_PUBLISH_OK:
            if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
            out->payload.publish_ok.location.group_id = val;
            pos += consumed;
            if (moqt_varint_decode(&in[pos], in_len - pos, &val, &consumed) < 0) return -1;
            out->payload.publish_ok.location.object_id = val;
            pos += consumed;
            break;
        default:
            break;
    }
    
    return (int)pos;
}

void moqt_buffer_init(moqt_buffer_t *buf, uint8_t *data, size_t capacity)
{
    buf->buffer = data;
    buf->capacity = capacity;
    buf->length = 0;
}

int moqt_buffer_append(moqt_buffer_t *buf, const uint8_t *data, size_t len)
{
    if (buf->length + len > buf->capacity) return -1;
    memcpy(&buf->buffer[buf->length], data, len);
    buf->length += len;
    return 0;
}