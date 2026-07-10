#pragma once

/*
 * Store-and-forward buffer for the drone / intermittent-link case.
 *
 * When the link is down (out of coverage, behind terrain, mid-handover), small
 * detection objects are buffered locally *with their original gen_ts_ms*; when
 * connectivity returns they are forwarded oldest-first. Preserving gen_ts keeps
 * Age-of-Information honest — the receiver sees exactly how stale a buffered
 * detection is (a person seen at GPS X, delivered 30 s later, is still useful).
 *
 * Only SMALL objects (AI results / detections / telemetry) are buffered; bulky
 * images are live-only (buffering 8 KB frames would blow MCU RAM and they matter
 * less than the events). On overflow the OLDEST buffered object is dropped
 * (recent detections kept) and counted, so the loss is observable, not silent.
 *
 * Dependency-free (host-testable, like presence/): no ESP-IDF/FreeRTOS deps.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SF_MAX_OBJECT_LEN 160U   /* fits an AI result (+GPS); NOT images */
#define SF_CAPACITY       128U   /* ring slots */

typedef struct {
    uint8_t  track;
    uint8_t  priority;
    uint32_t seq;
    uint32_t gen_ts_ms;          /* ORIGINAL generation time — preserved */
    uint16_t len;
    uint8_t  data[SF_MAX_OBJECT_LEN];
} sf_object_t;

typedef struct {
    sf_object_t slots[SF_CAPACITY];
    uint16_t head;               /* oldest */
    uint16_t count;
    uint32_t buffered_total;     /* lifetime pushes accepted */
    uint32_t forwarded_total;    /* lifetime pops */
    uint32_t dropped_total;      /* objects dropped on overflow (oldest) */
    uint32_t oversized_total;    /* rejected: payload > SF_MAX_OBJECT_LEN */
} store_forward_t;

void store_forward_init(store_forward_t *sf);

/* Buffer one object (link is down). Drops the oldest if full. Returns false if
 * the payload is too large to buffer (an image) — caller lets it go live-only. */
bool store_forward_push(store_forward_t *sf, uint8_t track, uint8_t priority,
                        uint32_t gen_ts_ms, uint32_t seq,
                        const uint8_t *data, uint16_t len);

/* Copy the oldest buffered object into *out and remove it. False if empty. */
bool store_forward_pop(store_forward_t *sf, sf_object_t *out);

static inline uint16_t store_forward_count(const store_forward_t *sf)
{
    return (sf != NULL) ? sf->count : 0U;
}
static inline bool store_forward_empty(const store_forward_t *sf)
{
    return store_forward_count(sf) == 0U;
}
