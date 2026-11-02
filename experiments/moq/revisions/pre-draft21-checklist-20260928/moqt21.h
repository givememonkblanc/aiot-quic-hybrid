#ifndef AIOT_MOQT21_H
#define AIOT_MOQT21_H
#include <stddef.h>
#include <stdint.h>
/* Draft-21 scoped wire codec. All readers: 1=complete, 0=need more, -1=invalid. */
#define MOQT21_ALPN "moqt-21"
#define MOQT21_CONTROL_CAP 2048
#define MOQT21_MAX_JPEG (256U*1024U)
/* draft-ietf-moq-transport-21 section 12.5. */
#define MOQT21_STREAM_RESET_DELIVERY_TIMEOUT 0x2U
size_t moqt21_vi_write(uint8_t *dst, size_t cap, uint64_t value);
int moqt21_vi_read(const uint8_t *src, size_t len, uint64_t *value, size_t *used);
size_t moqt21_message(uint8_t *dst,size_t cap,uint64_t type,const uint8_t *body,size_t len);
int moqt21_message_read(const uint8_t *src,size_t len,uint64_t *type,const uint8_t **body,size_t *body_len,size_t *used);
size_t moqt21_setup(uint8_t *dst,size_t cap,const char *authority);
/* Validate peer server SETUP options; rejects server PATH/AUTHORITY. */
int moqt21_server_setup(const uint8_t *body,size_t len);
/* Minimal unfiltered, zero-parameter subscription to known namespace. */
int moqt21_subscribe(const uint8_t *body,size_t len,uint64_t *request);
size_t moqt21_subscribe_ok(uint8_t *dst,size_t cap);
size_t moqt21_object_header(uint8_t *dst,size_t cap,uint64_t group,size_t payload_len);
int moqt21_subscribe_track(const uint8_t *,size_t,uint64_t *,unsigned *alias);
size_t moqt21_subscribe_ok_track(uint8_t *,size_t,unsigned alias);
size_t moqt21_object_header_track(uint8_t *,size_t,unsigned alias,uint64_t group,size_t len);
#endif
