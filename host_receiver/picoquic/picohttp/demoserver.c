/*
* Author: Christian Huitema
* Copyright (c) 2019, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WINDOWS
#include "wincompat.h"
#include "ws2ipdef.h"
#pragma warning(disable:4100)
#endif
#include "picoquic_internal.h"
#include "h3zero.h"
#include "democlient.h"
#include "demoserver.h"
#include "quicperf.h"
/* The HTTP 0.9 server code is used for early test of the QUIC transport functions. 
 * The simple server provides simple responses, precanned index files or randomly
 * generated content */

static const char* bad_request_message = "<html><head><title>Bad Request</title></head><body>Bad request. Why don't you try \"GET /456789\"?</body></html>";
static const uint8_t mqtt_connack[] = { 0x20, 0x02, 0x00, 0x00 };

/*
 * Receiver-side datagram log for the AIoT MQTT-over-QUIC experiments.
 * Every QUIC DATAGRAM carries a 12-byte object header (see firmware
 * src/components/common/include/aiot_object.h, little-endian):
 *   magic_ver(0xA1) track_id priority flags object_seq(u32) gen_ts_ms(u32)
 * Each datagram is appended to a CSV so per-track loss (seq gaps),
 * reordering and age-of-information can be computed offline.
 * Path override: AIOT_DGRAM_LOG env var; default ./dgram_recv.csv.
 */
#define AIOT_OBJECT_MAGIC_VER 0xA1U
#define AIOT_OBJECT_HEADER_SIZE 12U

static FILE* mqtt_dgram_log_file = NULL;
static int mqtt_dgram_log_failed = 0;

/*
 * Unified JSON-lines feed for the AIoT product backend/dashboard. Captures the
 * actual payloads (retained registration/state/availability PUBLISHes and the
 * heartbeat datagrams) so a backend can tail one file. Independent of the CSV
 * experiment logs above. Path override: AIOT_FEED_LOG (default aiot_feed.jsonl).
 */
static FILE* aiot_feed_file = NULL;
static int aiot_feed_failed = 0;

static void aiot_feed_json_string(FILE* f, const uint8_t* s, size_t len)
{
    /* Emit s as a JSON string body (no surrounding quotes), escaped. */
    for (size_t i = 0; i < len; i++) {
        int ch = s[i];
        if (ch == '"' || ch == '\\') {
            fprintf(f, "\\%c", ch);
        } else if (ch == '\n') {
            fprintf(f, "\\n");
        } else if (ch == '\r') {
            fprintf(f, "\\r");
        } else if (ch == '\t') {
            fprintf(f, "\\t");
        } else if (ch < 0x20) {
            fprintf(f, "\\u%04x", (unsigned)ch);
        } else {
            fputc(ch, f);
        }
    }
}

static FILE* aiot_feed_open(void)
{
    if (aiot_feed_file != NULL || aiot_feed_failed) {
        return aiot_feed_file;
    }
    const char* path = getenv("AIOT_FEED_LOG");
    if (path == NULL || path[0] == 0) {
        path = "aiot_feed.jsonl";
    }
    aiot_feed_file = fopen(path, "a");
    if (aiot_feed_file == NULL) {
        fprintf(stderr, "Cannot open AIoT feed %s\n", path);
        aiot_feed_failed = 1;
    }
    return aiot_feed_file;
}

/*
 * Live-view image reassembly. The firmware streams JPEG frames on the IMAGE
 * track (track_id 3) as chunked datagrams; we reassemble SOI..EOI into whole
 * JPEGs and atomically publish the latest one to AIOT_IMAGE_OUT (default
 * latest.jpg) for the backend's MJPEG endpoint. Single-device product setup,
 * so one global buffer is enough. Lost chunks corrupt a frame; we resync on
 * the next Start-Of-Image marker.
 */
#define AIOT_TRACK_IMAGE_ID 3U
#define AIOT_IMG_MAX 262144
static uint8_t aiot_img_buf[AIOT_IMG_MAX];
static size_t aiot_img_len;

static void aiot_image_write(const uint8_t* buf, size_t len)
{
    const char* path = getenv("AIOT_IMAGE_OUT");
    if (path == NULL || path[0] == 0) {
        path = "latest.jpg";
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (f == NULL) {
        return;
    }
    fwrite(buf, 1, len, f);
    fclose(f);
    rename(tmp, path);   /* atomic swap so readers never see a partial frame */
}

/*
 * Accuracy testbed: when AIOT_IMAGE_DIR is set, keep every reassembled frame
 * instead of only the latest one, named by the gen_ts its chunks carried. The
 * AI object generated from the same capture carries the same gen_ts, so a
 * host-side reference detector can label frame_<gen_ts>.jpg and join it with
 * the device's score without any clock alignment.
 */
static void aiot_image_archive(const uint8_t* buf, size_t len, uint32_t gen_ts_ms)
{
    const char* dir = getenv("AIOT_IMAGE_DIR");
    char path[512];
    FILE* f;

    if (dir == NULL || dir[0] == 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/frame_%010u.jpg", dir, gen_ts_ms);
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fwrite(buf, 1, len, f);
    fclose(f);
}

static void aiot_image_reassemble(const uint8_t* payload, size_t len,
    uint32_t gen_ts_ms, uint32_t seq, const char* cid)
{
    static uint32_t frame_ts, next_seq;
    static char frame_cid[2 * PICOQUIC_CONNECTION_ID_MAX_SIZE + 1];
    int start = len >= 2 && payload[0] == 0xff && payload[1] == 0xd8;
    if (start) {
        aiot_img_len = 0;
        frame_ts = gen_ts_ms;
        snprintf(frame_cid, sizeof(frame_cid), "%s", cid);
        next_seq = seq;
    }
    if ((!start && aiot_img_len == 0) || frame_ts != gen_ts_ms ||
        strcmp(frame_cid, cid) != 0 || next_seq != seq ||
        aiot_img_len + len > AIOT_IMG_MAX) {
        aiot_img_len = 0;
        return;
    }
    memcpy(aiot_img_buf + aiot_img_len, payload, len);
    aiot_img_len += len;
    next_seq = seq + 1;
    /* Only complete, contiguous chunks from one connection/frame may publish. */
    if (aiot_img_len >= 4 && aiot_img_buf[aiot_img_len-2] == 0xff &&
        aiot_img_buf[aiot_img_len-1] == 0xd9) {
        aiot_image_write(aiot_img_buf, aiot_img_len);
        aiot_image_archive(aiot_img_buf, aiot_img_len, gen_ts_ms);
        aiot_img_len = 0;
    }
}

/*
 * AI track (track_id 2): the serialized inference result for one streamed
 * frame — see src/components/common/include/ai_result.h. v2 carries
 * raw_score, the model's best score *before* its own detection threshold,
 * which is what an accuracy comparison needs (a thresholded 0 tells you
 * nothing about how close the model was). Path override: AIOT_AI_LOG.
 */
#define AIOT_TRACK_AI_ID 2U
#define AIOT_AI_RESULT_MAGIC 0xD1U
static FILE* aiot_ai_log_file = NULL;
static int aiot_ai_log_failed = 0;

static void aiot_ai_log(uint64_t recv_us, const char* cid_hex, uint32_t object_seq,
                        uint32_t gen_ts_ms, const uint8_t* p, size_t len)
{
    unsigned version, backend, det_count, raw_score, best_conf = 0;
    unsigned src_w, src_h, in_w, in_h, header_size;
    uint32_t source_seq;

    if (len < 16 || p[0] != AIOT_AI_RESULT_MAGIC) {
        return;
    }
    version = p[1];
    header_size = (version >= 2) ? 20u : 16u;
    if (len < header_size) {
        return;
    }
    backend = p[2];
    det_count = p[3];
    src_w = (unsigned)p[4] | ((unsigned)p[5] << 8);
    src_h = (unsigned)p[6] | ((unsigned)p[7] << 8);
    in_w = (unsigned)p[8] | ((unsigned)p[9] << 8);
    in_h = (unsigned)p[10] | ((unsigned)p[11] << 8);
    source_seq = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
        ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    raw_score = (version >= 2) ? p[16] : 0u;

    for (unsigned i = 0; i < det_count; i++) {
        size_t off = header_size + (size_t)i * 6u;
        if (off + 6u > len) {
            break;
        }
        if (p[off + 1] > best_conf) {
            best_conf = p[off + 1];
        }
    }
    if (version < 2) {
        raw_score = best_conf;   /* v1 had no pre-threshold score */
    }

    if (aiot_ai_log_file == NULL) {
        const char* path;
        if (aiot_ai_log_failed) {
            return;
        }
        path = getenv("AIOT_AI_LOG");
        if (path == NULL || path[0] == 0) {
            path = "ai_recv.csv";
        }
        aiot_ai_log_file = fopen(path, "a");
        if (aiot_ai_log_file == NULL) {
            fprintf(stderr, "Cannot open AI log %s\n", path);
            aiot_ai_log_failed = 1;
            return;
        }
        if (ftell(aiot_ai_log_file) == 0) {
            fprintf(aiot_ai_log_file,
                "recv_us,cid,object_seq,gen_ts_ms,version,backend,det_count,"
                "raw_score,best_conf,source_seq,src_w,src_h,in_w,in_h\n");
        }
    }

    fprintf(aiot_ai_log_file,
        "%" PRIu64 ",%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        recv_us, cid_hex, object_seq, gen_ts_ms, version, backend, det_count,
        raw_score, best_conf, source_seq, src_w, src_h, in_w, in_h);
    fflush(aiot_ai_log_file);
}

static void picoquic_mqtt_log_datagram(picoquic_cnx_t* cnx, const uint8_t* bytes, size_t length)
{
    uint64_t recv_us = picoquic_current_time();
    picoquic_connection_id_t icid = picoquic_get_initial_cnxid(cnx);
    char cid_hex[2 * PICOQUIC_CONNECTION_ID_MAX_SIZE + 1];
    size_t cid_len = (icid.id_len < PICOQUIC_CONNECTION_ID_MAX_SIZE) ? icid.id_len : PICOQUIC_CONNECTION_ID_MAX_SIZE;

    if (mqtt_dgram_log_file == NULL) {
        if (mqtt_dgram_log_failed) {
            return;
        }
        else {
            const char* path = getenv("AIOT_DGRAM_LOG");
            if (path == NULL || path[0] == 0) {
                path = "dgram_recv.csv";
            }
            mqtt_dgram_log_file = fopen(path, "a");
            if (mqtt_dgram_log_file == NULL) {
                fprintf(stderr, "Cannot open datagram log %s\n", path);
                mqtt_dgram_log_failed = 1;
                return;
            }
            if (ftell(mqtt_dgram_log_file) == 0) {
                fprintf(mqtt_dgram_log_file,
                    "recv_us,cid,valid,track_id,priority,flags,object_seq,gen_ts_ms,payload_len\n");
            }
        }
    }

    for (size_t i = 0; i < cid_len; i++) {
        (void)sprintf(&cid_hex[2 * i], "%02x", icid.id[i]);
    }
    cid_hex[2 * cid_len] = 0;

    if (length >= AIOT_OBJECT_HEADER_SIZE && bytes[0] == AIOT_OBJECT_MAGIC_VER) {
        uint32_t object_seq = (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8) |
            ((uint32_t)bytes[6] << 16) | ((uint32_t)bytes[7] << 24);
        uint32_t gen_ts_ms = (uint32_t)bytes[8] | ((uint32_t)bytes[9] << 8) |
            ((uint32_t)bytes[10] << 16) | ((uint32_t)bytes[11] << 24);
        fprintf(mqtt_dgram_log_file, "%" PRIu64 ",%s,1,%u,%u,%u,%u,%u,%zu\n",
            recv_us, cid_hex, bytes[1], bytes[2], bytes[3], object_seq, gen_ts_ms,
            length - AIOT_OBJECT_HEADER_SIZE);
        FILE* feed = aiot_feed_open();
        if (feed != NULL) {
            fprintf(feed, "{\"t\":%" PRIu64 ",\"cid\":\"%s\",\"kind\":\"datagram\",\"track\":%u,"
                "\"seq\":%u,\"gen_ts_ms\":%u,\"payload\":\"",
                recv_us, cid_hex, bytes[1], object_seq, gen_ts_ms);
            aiot_feed_json_string(feed, bytes + AIOT_OBJECT_HEADER_SIZE,
                length - AIOT_OBJECT_HEADER_SIZE);
            fprintf(feed, "\"}\n");
            fflush(feed);
        }
        if (bytes[1] == AIOT_TRACK_IMAGE_ID) {
            aiot_image_reassemble(bytes + AIOT_OBJECT_HEADER_SIZE,
                                  length - AIOT_OBJECT_HEADER_SIZE, gen_ts_ms, object_seq, cid_hex);
        }
        else if (bytes[1] == AIOT_TRACK_AI_ID) {
            aiot_ai_log(recv_us, cid_hex, object_seq, gen_ts_ms,
                        bytes + AIOT_OBJECT_HEADER_SIZE,
                        length - AIOT_OBJECT_HEADER_SIZE);
        }
    }
    else {
        /* v1 firmware or unframed payload: keep the arrival for rate stats */
        fprintf(mqtt_dgram_log_file, "%" PRIu64 ",%s,0,0,0,0,0,0,%zu\n",
            recv_us, cid_hex, length);
    }
    fflush(mqtt_dgram_log_file);
}

/*
 * Minimal MQTT 3.1.1 stream parser for the AIoT presence-sensor product.
 * Reassembles MQTT packets across QUIC stream frames per connection and:
 *   CONNECT  -> CONNACK
 *   PINGREQ  -> PINGRESP (keepalive support)
 *   PUBLISH  -> appended to CSV (AIOT_MQTT_LOG env, default mqtt_events.csv);
 *               bulk topics (aiot/image) are logged topic+length only.
 */
#define MQTT_PARSE_MAX_CNX 8
#define MQTT_PARSE_BUF_SIZE 4096

static const uint8_t mqtt_pingresp[] = { 0xD0, 0x00 };

typedef struct {
    picoquic_cnx_t* cnx;
    size_t buf_len;
    uint8_t buf[MQTT_PARSE_BUF_SIZE];
    /* Command downlink: the topic the device SUBSCRIBEd to (aiot/<id>/cmd)
     * and the stream it lives on, so the server can PUBLISH back to it. */
    char sub_topic[128];
    size_t sub_topic_len;
    uint64_t sub_stream_id;
} mqtt_parse_slot_t;

static mqtt_parse_slot_t mqtt_parse_slots[MQTT_PARSE_MAX_CNX];
static FILE* mqtt_event_log_file = NULL;
static int mqtt_event_log_failed = 0;

static mqtt_parse_slot_t* mqtt_parse_slot(picoquic_cnx_t* cnx)
{
    mqtt_parse_slot_t* free_slot = NULL;
    for (int i = 0; i < MQTT_PARSE_MAX_CNX; i++) {
        if (mqtt_parse_slots[i].cnx == cnx) {
            return &mqtt_parse_slots[i];
        }
        if (mqtt_parse_slots[i].cnx == NULL && free_slot == NULL) {
            free_slot = &mqtt_parse_slots[i];
        }
    }
    if (free_slot == NULL) {
        /* recycle the first slot; product setups have a single device */
        free_slot = &mqtt_parse_slots[0];
    }
    free_slot->cnx = cnx;
    free_slot->buf_len = 0;
    free_slot->sub_topic_len = 0;
    return free_slot;
}

/*
 * Command downlink: when the file named by AIOT_CMD_FILE (default
 * cmd_publish.txt) exists, its content is published once, QoS 0, to the
 * topic the device subscribed to, then the file is removed. Polled from the
 * uplink packet path, so worst-case latency is one heartbeat (~5s).
 * Example:  echo "ota http://192.168.0.38:8070/aiot_quic.bin" > cmd_publish.txt
 */
static void mqtt_send_pending_cmd(picoquic_cnx_t* cnx, mqtt_parse_slot_t* slot)
{
    if (slot->sub_topic_len == 0) {
        return;
    }
    const char* path = getenv("AIOT_CMD_FILE");
    if (path == NULL) {
        path = "cmd_publish.txt";
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    char payload[300];
    size_t n = fread(payload, 1, sizeof(payload) - 1, f);
    fclose(f);
    while (n > 0 && (payload[n - 1] == '\n' || payload[n - 1] == '\r')) {
        n--;
    }
    if (n == 0) {
        return;
    }
    remove(path);

    uint8_t pkt[512];
    size_t topic_len = slot->sub_topic_len;
    size_t remaining = 2 + topic_len + n;
    size_t pos = 0;
    pkt[pos++] = 0x30; /* PUBLISH, QoS 0 */
    size_t r = remaining;
    do {
        uint8_t b = (uint8_t)(r & 0x7f);
        r >>= 7;
        if (r != 0) {
            b |= 0x80;
        }
        pkt[pos++] = b;
    } while (r != 0);
    pkt[pos++] = (uint8_t)(topic_len >> 8);
    pkt[pos++] = (uint8_t)(topic_len & 0xff);
    memcpy(pkt + pos, slot->sub_topic, topic_len);
    pos += topic_len;
    memcpy(pkt + pos, payload, n);
    pos += n;

    (void)picoquic_add_to_stream_with_ctx(cnx, slot->sub_stream_id, pkt, pos, 0, NULL);
    fprintf(stdout, "MQTT cmd -> %.*s: %.*s\n",
        (int)topic_len, slot->sub_topic, (int)n, payload);
    fflush(stdout);
}

static void mqtt_parse_slot_release(picoquic_cnx_t* cnx)
{
    for (int i = 0; i < MQTT_PARSE_MAX_CNX; i++) {
        if (mqtt_parse_slots[i].cnx == cnx) {
            mqtt_parse_slots[i].cnx = NULL;
            mqtt_parse_slots[i].buf_len = 0;
        }
    }
}

static void picoquic_mqtt_log_publish(picoquic_cnx_t* cnx, const char* topic,
    size_t topic_len, const uint8_t* payload, size_t payload_len)
{
    if (mqtt_event_log_file == NULL) {
        if (mqtt_event_log_failed) {
            return;
        }
        const char* path = getenv("AIOT_MQTT_LOG");
        if (path == NULL || path[0] == 0) {
            path = "mqtt_events.csv";
        }
        mqtt_event_log_file = fopen(path, "a");
        if (mqtt_event_log_file == NULL) {
            fprintf(stderr, "Cannot open MQTT event log %s\n", path);
            mqtt_event_log_failed = 1;
            return;
        }
        if (ftell(mqtt_event_log_file) == 0) {
            fprintf(mqtt_event_log_file, "recv_us,cid,topic,payload_len,payload\n");
        }
    }

    picoquic_connection_id_t icid = picoquic_get_initial_cnxid(cnx);
    char cid_hex[2 * PICOQUIC_CONNECTION_ID_MAX_SIZE + 1];
    size_t cid_len = (icid.id_len < PICOQUIC_CONNECTION_ID_MAX_SIZE) ? icid.id_len : PICOQUIC_CONNECTION_ID_MAX_SIZE;
    for (size_t i = 0; i < cid_len; i++) {
        (void)sprintf(&cid_hex[2 * i], "%02x", icid.id[i]);
    }
    cid_hex[2 * cid_len] = 0;

    int is_bulk = (topic_len >= 10 && memcmp(topic, "aiot/image", 10) == 0);
    fprintf(mqtt_event_log_file, "%" PRIu64 ",%s,%.*s,%zu,\"",
        picoquic_current_time(), cid_hex, (int)topic_len, topic, payload_len);
    if (!is_bulk) {
        size_t log_len = payload_len < 2048 ? payload_len : 2048;
        for (size_t i = 0; i < log_len; i++) {
            int ch = payload[i];
            if (ch == '"') {
                fputs("\"\"", mqtt_event_log_file);
            } else if (ch >= 0x20 && ch < 0x7f) {
                fputc(ch, mqtt_event_log_file);
            } else {
                fprintf(mqtt_event_log_file, "\\x%02x", (unsigned)ch);
            }
        }
    }
    fputs("\"\n", mqtt_event_log_file);
    fflush(mqtt_event_log_file);

    FILE* feed = aiot_feed_open();
    if (feed != NULL) {
        fprintf(feed, "{\"t\":%" PRIu64 ",\"cid\":\"%s\",\"kind\":\"publish\",\"topic\":\"",
            picoquic_current_time(), cid_hex);
        aiot_feed_json_string(feed, (const uint8_t*)topic, topic_len);
        fprintf(feed, "\",\"payload\":\"");
        if (!is_bulk) {
            aiot_feed_json_string(feed, payload, payload_len < 2048 ? payload_len : 2048);
        }
        fprintf(feed, "\"}\n");
        fflush(feed);
    }
}

/* Returns the total packet length when the buffer holds a complete MQTT
 * packet, 0 when more data is needed, (size_t)-1 on malformed input. */
static size_t mqtt_packet_length(const uint8_t* buf, size_t len)
{
    if (len < 2) {
        return 0;
    }
    size_t remaining = 0;
    size_t shift = 0;
    size_t pos = 1;
    for (;;) {
        if (pos >= len) {
            return 0;
        }
        uint8_t byte = buf[pos++];
        remaining |= (size_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
        if (shift > 21) {
            return (size_t)-1;
        }
    }
    return pos + remaining;
}

static int picoquic_mqtt_handle_packet(picoquic_cnx_t* cnx, uint64_t stream_id,
    mqtt_parse_slot_t* slot, const uint8_t* pkt, size_t pkt_len)
{
    uint8_t type = (pkt[0] >> 4) & 0x0F;

    switch (type) {
    case 1: /* CONNECT */
        return picoquic_add_to_stream_with_ctx(cnx, stream_id,
            mqtt_connack, sizeof(mqtt_connack), 0, NULL);
    case 12: /* PINGREQ */
        return picoquic_add_to_stream_with_ctx(cnx, stream_id,
            mqtt_pingresp, sizeof(mqtt_pingresp), 0, NULL);
    case 8: { /* SUBSCRIBE: remember the (single) topic filter, ack it */
        size_t pos = 1;
        while (pos < pkt_len && (pkt[pos] & 0x80) != 0) {
            pos++;
        }
        pos++;
        if (pos + 2 > pkt_len) {
            return 0;
        }
        uint8_t pid_hi = pkt[pos];
        uint8_t pid_lo = pkt[pos + 1];
        pos += 2;
        if (pos + 2 <= pkt_len) {
            size_t tl = ((size_t)pkt[pos] << 8) | pkt[pos + 1];
            pos += 2;
            if (pos + tl <= pkt_len && tl < sizeof(slot->sub_topic)) {
                memcpy(slot->sub_topic, &pkt[pos], tl);
                slot->sub_topic[tl] = '\0';
                slot->sub_topic_len = tl;
                slot->sub_stream_id = stream_id;
                fprintf(stdout, "MQTT SUBSCRIBE %s\n", slot->sub_topic);
                fflush(stdout);
            }
        }
        uint8_t suback[5] = { 0x90, 0x03, pid_hi, pid_lo, 0x00 };
        return picoquic_add_to_stream_with_ctx(cnx, stream_id,
            suback, sizeof(suback), 0, NULL);
    }
    case 3: { /* PUBLISH (QoS 0) */
        uint8_t qos = (pkt[0] >> 1) & 0x03;
        /* skip fixed header */
        size_t pos = 1;
        while (pos < pkt_len && (pkt[pos] & 0x80) != 0) {
            pos++;
        }
        pos++;
        if (pos + 2 > pkt_len) {
            return 0;
        }
        size_t topic_len = ((size_t)pkt[pos] << 8) | pkt[pos + 1];
        pos += 2;
        if (pos + topic_len > pkt_len) {
            return 0;
        }
        const char* topic = (const char*)&pkt[pos];
        pos += topic_len;
        if (qos > 0) {
            pos += 2; /* packet identifier */
        }
        if (pos > pkt_len) {
            return 0;
        }
        picoquic_mqtt_log_publish(cnx, topic, topic_len, &pkt[pos], pkt_len - pos);
        return 0;
    }
    default:
        return 0;
    }
}

int picoquic_mqtt_server_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    (void)callback_ctx;
    (void)v_stream_ctx;

    switch (fin_or_event) {
    case picoquic_callback_almost_ready:
    case picoquic_callback_ready:
    case picoquic_callback_version_negotiation:
    case picoquic_callback_stateless_reset:
    case picoquic_callback_stop_sending:
    case picoquic_callback_stream_reset:
        return 0;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
        mqtt_parse_slot_release(cnx);
        return 0;
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (length > 0 && bytes != NULL) {
            mqtt_parse_slot_t* slot = mqtt_parse_slot(cnx);
            if (slot->buf_len + length > sizeof(slot->buf)) {
                /* overflow: drop buffered state, try to resync on next packet */
                slot->buf_len = 0;
                if (length > sizeof(slot->buf)) {
                    return 0;
                }
            }
            memcpy(slot->buf + slot->buf_len, bytes, length);
            slot->buf_len += length;

            for (;;) {
                size_t pkt_len = mqtt_packet_length(slot->buf, slot->buf_len);
                if (pkt_len == 0) {
                    break; /* need more data */
                }
                if (pkt_len == (size_t)-1) {
                    slot->buf_len = 0; /* malformed: resync */
                    break;
                }
                if (pkt_len > slot->buf_len) {
                    break; /* incomplete */
                }
                (void)picoquic_mqtt_handle_packet(cnx, stream_id, slot, slot->buf, pkt_len);
                memmove(slot->buf, slot->buf + pkt_len, slot->buf_len - pkt_len);
                slot->buf_len -= pkt_len;
                mqtt_send_pending_cmd(cnx, slot);
            }
        }
        return 0;
    case picoquic_callback_datagram:
        if (bytes != NULL) {
            picoquic_mqtt_log_datagram(cnx, bytes, length);
        }
        return 0;
    default:
        return 0;
    }
}

static char* strip_endofline(char* buf, size_t bufmax, char const* line)
{
    for (size_t i = 0; i < bufmax; i++) {
        int c = line[i];

        if (c == 0 || c == '\r' || c == '\n') {
            buf[i] = 0;
            break;
        }
        else {
            buf[i] = (char) c;
        }
    }

    buf[bufmax - 1] = 0;
    return buf;
}


static picoquic_h09_server_callback_ctx_t* first_server_callback_create_context(picohttp_server_parameters_t* param)
{
    picoquic_h09_server_callback_ctx_t* ctx = (picoquic_h09_server_callback_ctx_t*)
        malloc(sizeof(picoquic_h09_server_callback_ctx_t));

    if (ctx != NULL) {
        memset(ctx, 0, sizeof(picoquic_h09_server_callback_ctx_t));

        h3zero_init_stream_tree(&ctx->h3_stream_tree);

        if (param != NULL) {
            ctx->path_table = param->path_table;
            ctx->path_table_nb = param->path_table_nb;
            ctx->web_folder = param->web_folder;
        }
    }

    return ctx;
}

static void picoquic_h09_server_callback_delete_context(picoquic_h09_server_callback_ctx_t* ctx)
{

    picosplay_empty_tree(&ctx->h3_stream_tree);

    free(ctx);
}


static int picoquic_h09_server_parse_method(uint8_t* command, size_t command_length, size_t * consumed)
{
    int byte_index = 0;
    int ret = -1;

    if (command_length >= 3 && (command[0] == 'G' || command[0] == 'g') && (command[1] == 'E' || command[1] == 'e') && (command[2] == 'T' || command[2] == 't')) {
        ret = 0;
        byte_index = 3;
    } else if (command_length >= 4 && (command[0] == 'P' || command[0] == 'p') && (command[1] == 'O' || command[1] == 'o') && (command[2] == 'S' || command[2] == 's') && (command[3] == 'T' || command[3] == 't')) {
        ret = 1;
        byte_index = 4;
    }

    if (consumed) {
        *consumed = byte_index;
    }

    return ret;
}

static void picoquic_h09_server_parse_protocol(uint8_t* command, size_t command_length, int * proto, size_t * consumed)
{
    size_t byte_index = (command_length > 0)?command_length -1:0;
    size_t last_proto_index;
    int space_count = 0;

    *proto = 0;

    /* skip white space at the end */
    for (;;) {
        int c = command[byte_index];

        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            space_count++;
            if (byte_index > 0) {
                byte_index--;
            }
            else {
                break;
            }
        }
        else {
            break;
        }
    }
    *consumed = space_count;
    last_proto_index = byte_index;

    /* find non space char */
    while (byte_index > 0) {
        int c = command[byte_index];

        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            byte_index++;
            break;
        }
        else {
            byte_index--;
        }
    }

    /* Parse protocol version */
    if (last_proto_index - byte_index == 7 &&
        command[byte_index + 6] == '.' &&
        command[byte_index + 4] == '/' &&
        (command[byte_index + 3] == 'p' || command[byte_index + 3] == 'P') &&
        (command[byte_index + 2] == 't' || command[byte_index + 2] == 'T') &&
        (command[byte_index + 1] == 't' || command[byte_index + 1] == 'T') &&
        (command[byte_index] == 'h' || command[byte_index] == 'H')) {
        int bad_version = 0;
        if (command[byte_index + 5] == '1' && (command[byte_index + 7] == '0' || command[byte_index + 7] == '1')) {
            *proto = 1;
        }
        else if (command[byte_index + 5] == '0' && command[byte_index + 7] == '9') {
            *proto = 0;
        }
        else {
            bad_version = 1;
        }

        if (!bad_version) {
            *consumed += 8;

            if (byte_index > 0) {
                byte_index--;
                while (byte_index > 0 && (command[byte_index] == ' ' || command[byte_index] == '\t')) {
                    byte_index--;
                    *consumed += 1;
                }
            }
        }
    }
}

static int picohttp_server_parse_commandline(uint8_t* command, size_t command_length, h3zero_stream_ctx_t* stream_ctx)
{
    int ret = 0;
    size_t consumed;

    /* Find first line of command, ignore the rest */
    for (size_t i = 0; i < command_length; i++) {
        if (command[i] == '\r' || command[i] == '\n') {
            command_length = i;
            break;
        }
    }

    /* Parse protocol version and strip white spaces at the end of the command */
    picoquic_h09_server_parse_protocol(command, command_length, &stream_ctx->ps.hq.proto, &consumed);
    command_length -= consumed;

    /* parse the method */
    stream_ctx->ps.hq.method = picoquic_h09_server_parse_method(command, command_length, &consumed);

    /* Skip white spaces between method and path, and copy path if present */
    if (stream_ctx->ps.hq.method < 0) {
        ret = -1;
    }
    else {
        /* Skip at list one space */
        while (command_length > consumed && (command[consumed] == ' ' || command[consumed] == '\t')) {
            consumed++;
        }

        if (consumed >= command_length) {
            ret = -1;
        }
        else {
            size_t path_length = command_length - consumed;
            uint8_t* path = (uint8_t*)malloc(path_length + 1);

            if (path != NULL) {
                memcpy(path, command + consumed, path_length);
                path[path_length] = 0;
                stream_ctx->ps.hq.path = path;
                stream_ctx->ps.hq.path_length = path_length;
            }
            else {
                ret = -1;
            }
        }
    }

    return ret;
}

/*
 * Process the incoming data. 
 * We can expect the following:
 * - Initial command line: {GET|POST} <path> [HTTP/{0.9|1.0|1.1}] /r/n
 * - Additional command lines concluded with /r/n
 * - Empty line: /r/n
 * - Posted data
 * This can be interrupted at any time by a FIN mark. In the case of the 
 * GET command, there should not be any posted data. 
 * The server should parse the initial line to gather the type of command
 * and the name of the document. It should then parse data until the fin
 * mark is received.
 * The additional headers are ignored.
 * The amount of posted data is counted, will be used to prepare the response.
 *
 * The response is sent after the FIN is received (POST) or after the 
 * header line is fully parsed (GET).
 */

int picoquic_h09_server_process_data_header(
    const uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event,
    h3zero_stream_ctx_t* stream_ctx,
    size_t * r_processed)
{
    int ret = 0;
    size_t processed = 0;

    while (ret == 0 && processed < length) {
        if (stream_ctx->ps.hq.status == picohttp_server_stream_status_none) {
            /* If the command has not been received yet, try to process it */
            int crlf_present = 0;

            while (processed < length && crlf_present == 0) {
                if (bytes[processed] == '\r') {
                    /* Ignore \r, so end of header is either CRLF/CRLF, of just LF/LF, or maybe LF/CR/LF */
                }
                else if (bytes[processed] == '\n') {
                    crlf_present = 1;
                }
                else if (stream_ctx->ps.hq.command_length < sizeof(stream_ctx->frame) - 1) {
                    stream_ctx->frame[stream_ctx->ps.hq.command_length++] = bytes[processed];
                }
                else {
                    /* Too much data */
                    stream_ctx->ps.hq.method = -1;
                    ret = -1;
                    break;
                }
                processed++;
            }

            if (crlf_present) {
                stream_ctx->ps.hq.status = picohttp_server_stream_status_crlf;
            }

            if (crlf_present || fin_or_event == picoquic_callback_stream_fin) {
                /* Parse the command */
                ret = picohttp_server_parse_commandline(stream_ctx->frame, stream_ctx->ps.hq.command_length, stream_ctx);
            }
        }
        else if (stream_ctx->ps.hq.status == picohttp_server_stream_status_crlf) {
            if (bytes[processed] == '\n') {
                /* empty line */
                stream_ctx->ps.hq.status = picohttp_server_stream_status_receiving;
            }
            else if (bytes[processed] != '\r') {
                stream_ctx->ps.hq.status = picohttp_server_stream_status_header;
            }
            processed++;
        }
        else if (stream_ctx->ps.hq.status == picohttp_server_stream_status_header) {
            if (bytes[processed] == '\n') {
                stream_ctx->ps.hq.status = picohttp_server_stream_status_crlf;
            }
            processed++;
        }
        else
        {
            break;
        }
    }

    *r_processed = processed;
    return ret;
}

int picoquic_h09_server_process_data(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, 
    picoquic_h09_server_callback_ctx_t* app_ctx,
    h3zero_stream_ctx_t* stream_ctx)
{
    int ret = 0;
    size_t processed = 0;

    ret = picoquic_h09_server_process_data_header(bytes, length, fin_or_event, stream_ctx, &processed);

    if (ret == 0 && processed < length) {
        if (stream_ctx->ps.hq.status == picohttp_server_stream_status_receiving) {
            /* Received data for a POST command. */
            size_t available = length - processed;

            if (stream_ctx->post_received == 0 && available > 0) {
                int path_item = picohttp_find_path_item(stream_ctx->ps.hq.path, stream_ctx->ps.hq.path_length, app_ctx->path_table, app_ctx->path_table_nb);
                if (path_item >= 0) {
                    stream_ctx->path_callback = app_ctx->path_table[path_item].path_callback;
                    stream_ctx->path_callback(cnx, (uint8_t*)stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, picohttp_callback_post, stream_ctx, 
                        app_ctx->path_table[path_item].path_app_ctx);
                }
                stream_ctx->post_received += available;
                (void)picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx);
            }

            if (stream_ctx->path_callback != NULL) {
                /* pass data to selected API */
                ret = stream_ctx->path_callback(cnx, bytes + processed, available, picohttp_callback_post_data, stream_ctx, stream_ctx->path_callback_ctx);
                /* TODO-POST: how to handle errors ?*/
            }
            stream_ctx->post_received += available;
            processed = length;
        }
        else {
            /* No more processing expected on this stream. */
            processed = length;
        }
    }

    /* if FIN present, process request through http 0.9 */
    if (ret == 0 && stream_ctx->ps.hq.status != picohttp_server_stream_status_finished) {
        if (fin_or_event == picoquic_callback_stream_fin || (stream_ctx->ps.hq.method == 0 && stream_ctx->ps.hq.status == picohttp_server_stream_status_crlf)) {
            char buf[1024];
            uint8_t post_response[512];
            int is_bad_request = 0;
            int is_not_found = 0;

            stream_ctx->frame[stream_ctx->ps.hq.command_length] = 0;
            stream_ctx->ps.hq.status = picohttp_server_stream_status_finished;

            picoquic_log_app_message(cnx, "Server CB, Stream: %" PRIu64 ", Processing command: %s\n",
                stream_id, strip_endofline(buf, sizeof(buf), (char*)&stream_ctx->frame));

            if (stream_ctx->ps.hq.method == 0) {
                int file_error = 0;
                if (h3zero_server_parse_path(stream_ctx->ps.hq.path, stream_ctx->ps.hq.path_length,
                    &stream_ctx->echo_length, &stream_ctx->file_path, app_ctx->web_folder, &file_error)) {
                    char log_text[256];
                    picoquic_log_app_message(cnx, "Cannot find file for path: <%s> in folder <%s>, error: 0x%x",
                        picoquic_uint8_to_str(log_text, 256, stream_ctx->ps.hq.path, stream_ctx->ps.hq.path_length),
                        (app_ctx->web_folder==NULL)?"NULL": app_ctx->web_folder, file_error);
                    is_not_found = 1;
                }
            }
            else if (stream_ctx->ps.hq.method == 1) {
                if (stream_ctx->post_received == 0) {
                    int path_item = picohttp_find_path_item(stream_ctx->ps.hq.path, stream_ctx->ps.hq.path_length, app_ctx->path_table, app_ctx->path_table_nb);
                    if (path_item >= 0) {
                        /* TODO-POST: move this code to post-fin callback.*/
                        stream_ctx->path_callback = app_ctx->path_table[path_item].path_callback;
                        stream_ctx->path_callback(cnx, (uint8_t*)stream_ctx->ps.stream_state.header.path, stream_ctx->ps.stream_state.header.path_length, picohttp_callback_post, 
                            stream_ctx, app_ctx->path_table[path_item].path_app_ctx);
                    }
                }

                if (stream_ctx->path_callback != NULL) {
                    stream_ctx->response_length = stream_ctx->path_callback(cnx, post_response, sizeof(post_response), picohttp_callback_post_fin, stream_ctx,
                        stream_ctx->path_callback_ctx);
                    if (stream_ctx->response_length == 0) {
                        is_bad_request = 1;
                    }
                }
                else {
                    /* Prepare generic POST response */
                    size_t message_length = 0;
                    (void)picoquic_sprintf((char*)post_response, sizeof(post_response), &message_length, h3zero_server_post_response_page, (int)stream_ctx->post_received);
                    stream_ctx->response_length = message_length;
                }
                stream_ctx->echo_length = 0;
            }
            else {
                is_bad_request = 1;
            }

            if (is_bad_request || is_not_found) {
                /* If this is HTTP1, send an HTTP1 OK message, with the appropriate content type */
                if (stream_ctx->ps.hq.proto != 0) {
                    size_t header_length = 0;

                    picoquic_sprintf(buf, sizeof(buf), &header_length, "%s\r\n\r\n",
                        (is_not_found) ? "404 Not Found" : "400 Bad Request");
                    picoquic_add_to_stream_with_ctx(cnx, stream_id, (uint8_t*)buf, header_length, 0, (void*)stream_ctx);
                }

                picoquic_log_app_message(cnx, "Server CB, Stream: %" PRIu64 ", Reply with bad request message after command: %s\n",
                    stream_id, strip_endofline(buf, sizeof(buf), (char*)&stream_ctx->frame));

                stream_ctx->response_length = strlen(bad_request_message);
                (void)picoquic_add_to_stream_with_ctx(cnx, stream_ctx->stream_id, (const uint8_t*)bad_request_message,
                    (size_t)stream_ctx->response_length, 1, (void*)stream_ctx);
            }
            else {
                /* If this is HTTP1, send an HTTP1 OK message, with the appropriate content type */
                if (stream_ctx->ps.hq.proto != 0) {
                    size_t header_length = 0;

                    picoquic_sprintf(buf, sizeof(buf), &header_length, "200 OK\r\nContent-Type:%s\r\n\r\n",
                        (stream_ctx->echo_length == 0 || stream_ctx->ps.hq.method == 1) ? "text/plain" : "test/html");
                    picoquic_add_to_stream_with_ctx(cnx, stream_id, (uint8_t*)buf, header_length, 0, (void*)stream_ctx);
                }

                if (stream_ctx->response_length == 0 && stream_ctx->echo_length == 0) {
                    /* Send the canned index.html response */
                    stream_ctx->response_length = strlen(h3zero_server_default_page);
                    picoquic_add_to_stream_with_ctx(cnx, stream_id, (uint8_t*)h3zero_server_default_page,
                        (size_t)stream_ctx->response_length, 1, (void*)stream_ctx);
                }
                else if (stream_ctx->echo_length == 0 && stream_ctx->response_length < sizeof(post_response)) {
                    /* For short responses, post directly.
                     * TODO-POST: for long responses, we expect that the application
                     * will have set a data provision shortcut. Verify that! */
                    picoquic_add_to_stream_with_ctx(cnx, stream_id, post_response,
                        (size_t)stream_ctx->response_length, 1, (void*)stream_ctx);
                }
                else {
                    picoquic_mark_active_stream(cnx, stream_ctx->stream_id, 1, stream_ctx);
                }
            }
        }
        else if (stream_ctx->response_length == 0 && stream_ctx->echo_length == 0 && stream_ctx->ps.hq.method == 0) {
            char buf[256];
            if (stream_ctx->ps.hq.command_length < sizeof(stream_ctx->frame)){
                stream_ctx->frame[stream_ctx->ps.hq.command_length] = 0;

                picoquic_log_app_message(cnx, "Server CB, Stream: %" PRIu64 ", Partial command: %s\n",
                    stream_id, strip_endofline(buf, sizeof(buf), (char*)&stream_ctx->frame));
            }
            else {

            }
        }
    }

    return ret;
}

int picoquic_h09_server_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    picoquic_h09_server_callback_ctx_t* ctx = NULL;
    h3zero_stream_ctx_t* stream_ctx = (h3zero_stream_ctx_t*)v_stream_ctx;

    if (picoquic_cnx_is_still_logging(cnx)) {
        picoquic_log_app_message(cnx, "Server CB, Stream: %" PRIu64 ", %" PRIst " bytes, fin=%d\n",
            stream_id, length, fin_or_event);
    }

    if (callback_ctx == NULL || callback_ctx == picoquic_get_default_callback_context(cnx->quic)) {
        picoquic_h09_server_callback_ctx_t* new_ctx = first_server_callback_create_context((picohttp_server_parameters_t*)callback_ctx);
        if (new_ctx == NULL) {
            /* cannot handle the connection */
            picoquic_log_app_message(cnx, "Memory error, cannot allocate application context\n");
            picoquic_close(cnx, PICOQUIC_ERROR_MEMORY);
            return 0;
        }
        else {
            picoquic_set_callback(cnx, picoquic_h09_server_callback, new_ctx);
            ctx = new_ctx;
        }
    }
    else {
        ctx = (picoquic_h09_server_callback_ctx_t*)callback_ctx;
    }

    switch (fin_or_event) {
    case picoquic_callback_almost_ready:
    case picoquic_callback_ready:
        return 0;
    case picoquic_callback_version_negotiation:
        return 0;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
            if (ctx != NULL ) {
                picoquic_h09_server_callback_delete_context(ctx);
                picoquic_set_callback(cnx, NULL, NULL);
            }
            fflush(stdout);
            return 0;
    default:
        break;
    }

    if (stream_ctx == NULL) {
        stream_ctx = h3zero_find_or_create_stream(cnx, stream_id, ctx, 1, 0);
    }

    switch (fin_or_event) {
    case picoquic_callback_stop_sending:
        /* TODO-POST: notify callback. */
        stream_ctx->ps.hq.status = picohttp_server_stream_status_finished;
        picoquic_reset_stream(cnx, stream_id, 0);
        picoquic_log_app_message(cnx, "Server CB, Stop Sending Stream: %" PRIu64 ", resetting the local stream.\n", stream_id);
        if (stream_ctx != NULL && stream_ctx->F != NULL) {
            stream_ctx->F = picoquic_file_close(stream_ctx->F);
        }
        return 0;
    case picoquic_callback_stream_reset:
        /* TODO-POST: notify callback. */
        stream_ctx->ps.hq.status = picohttp_server_stream_status_finished;
        picoquic_reset_stream(cnx, stream_id, 0);
        picoquic_log_app_message(cnx, "Server CB, Reset Stream: %" PRIu64 ", resetting the local stream.\n", stream_id);

        if (stream_ctx != NULL && stream_ctx->F != NULL) {
            stream_ctx->F = picoquic_file_close(stream_ctx->F);
        }
        return 0;
    case picoquic_callback_prepare_to_send:
            /* Used for active streams */
            if (stream_ctx == NULL) {
                /* Unexpected */
                picoquic_reset_stream(cnx, stream_id, 0);
                return 0;
            }
            else {
                if (stream_ctx->path_callback != NULL) {
                    return stream_ctx->path_callback(cnx, bytes, length, picohttp_callback_provide_data, stream_ctx, stream_ctx->path_callback_ctx);
                }
                else {
                    /* TODO-POST: notify callback. */
                    int ret = h3zero_server_prepare_to_send((void*)bytes, length, stream_ctx);
                    if (stream_ctx->echo_sent >= stream_ctx->echo_length) {
                        h3zero_delete_stream(cnx, ctx, stream_ctx);
                    }
                    return ret;
                }
            }
    default:
        break;
    }

    if (fin_or_event == picoquic_callback_stream_gap) {
        /* We do not support this, yet */
        stream_ctx->ps.hq.status = picohttp_server_stream_status_finished;
        picoquic_reset_stream(cnx, stream_id, PICOQUIC_TRANSPORT_PROTOCOL_VIOLATION);
        picoquic_log_app_message(cnx, "Server CB, Stream: %" PRIu64 ", RESET, stream gaps not supported\n", stream_id);
        return 0;
    }
    else if (fin_or_event == picoquic_callback_stream_data || fin_or_event == picoquic_callback_stream_fin) {
        /* Data processing includes setting up post/get callback if needed */
        if (picoquic_h09_server_process_data(cnx, stream_id, bytes, length, fin_or_event, ctx, stream_ctx)) {
            /* something bad happened. */
        }
    } else {
        /* Unknown event */
        /* TODO-POST: notify callback. */
        stream_ctx->ps.hq.status = picohttp_server_stream_status_finished;
        picoquic_reset_stream(cnx, stream_id, PICOQUIC_TRANSPORT_INTERNAL_ERROR);
        picoquic_log_app_message(cnx, "Server CB, Stream: %" PRIu64 ", unexpected event\n", stream_id);
    }

    /* that's it */
    return 0;
}

/* Callback from the TLS stack upon receiving a list of proposed ALPN in the Client Hello */
size_t picoquic_demo_server_callback_select_alpn(picoquic_quic_t* quic, picoquic_iovec_t* list, size_t count)
{
    size_t ret = count;
    picoquic_alpn_enum alpn_code = picoquic_alpn_undef;
    picoquic_cnx_t* cnx = quic->cnx_in_progress;

    for (size_t i = 0; i < count; i++) {
        if ((alpn_code = picoquic_parse_alpn_nz((const char *)list[i].base, list[i].len)) != picoquic_alpn_undef) {
            ret = i;
            break;
        }
    }

    if (alpn_code != picoquic_alpn_undef && cnx != NULL) {
        void* default_callback_ctx = picoquic_get_default_callback_context(quic);
        switch (alpn_code) {
        case picoquic_alpn_http_3:
            picoquic_set_callback(cnx, h3zero_callback, default_callback_ctx);
            break;
        case picoquic_alpn_quicperf:
            picoquic_set_callback(cnx, quicperf_callback, default_callback_ctx);
            break;
        case picoquic_alpn_http_0_9:
            picoquic_set_callback(cnx, picoquic_h09_server_callback, default_callback_ctx);
            break;
        case picoquic_alpn_mqtt:
            picoquic_set_callback(cnx, picoquic_mqtt_server_callback, default_callback_ctx);
            break;
        default:
            break;
        }
    }

    return ret;
}
