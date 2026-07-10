#include "store_forward.h"

#include <string.h>

void store_forward_init(store_forward_t *sf)
{
    if (sf == NULL) {
        return;
    }
    memset(sf, 0, sizeof(*sf));
}

bool store_forward_push(store_forward_t *sf, uint8_t track, uint8_t priority,
                        uint32_t gen_ts_ms, uint32_t seq,
                        const uint8_t *data, uint16_t len)
{
    if (sf == NULL || data == NULL) {
        return false;
    }
    if (len > SF_MAX_OBJECT_LEN) {
        sf->oversized_total++;   /* an image / too-big object — not buffered */
        return false;
    }

    /* Drop the oldest to make room (keep the most recent detections). */
    if (sf->count == SF_CAPACITY) {
        sf->head = (uint16_t)((sf->head + 1U) % SF_CAPACITY);
        sf->count--;
        sf->dropped_total++;
    }

    uint16_t tail = (uint16_t)((sf->head + sf->count) % SF_CAPACITY);
    sf_object_t *o = &sf->slots[tail];
    o->track = track;
    o->priority = priority;
    o->seq = seq;
    o->gen_ts_ms = gen_ts_ms;
    o->len = len;
    memcpy(o->data, data, len);

    sf->count++;
    sf->buffered_total++;
    return true;
}

bool store_forward_pop(store_forward_t *sf, sf_object_t *out)
{
    if (sf == NULL || out == NULL || sf->count == 0U) {
        return false;
    }
    *out = sf->slots[sf->head];
    sf->head = (uint16_t)((sf->head + 1U) % SF_CAPACITY);
    sf->count--;
    sf->forwarded_total++;
    return true;
}
