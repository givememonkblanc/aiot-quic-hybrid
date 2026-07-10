/*
 * Host unit test for the store-and-forward buffer (drone / intermittent link).
 * Verifies: buffering while "down", FIFO drain preserving gen_ts, overflow
 * drops oldest (counted), oversized rejected. Build/run: run_store_forward_test.sh
 */
#include <stdio.h>
#include <string.h>

#include "store_forward.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
    store_forward_t sf;
    store_forward_init(&sf);
    CHECK(store_forward_empty(&sf), "starts empty");

    /* Link down: buffer 3 detections with distinct gen_ts. */
    const char *p = "det";
    store_forward_push(&sf, 2, 0, 1000, 10, (const uint8_t *)p, 3);
    store_forward_push(&sf, 2, 0, 1050, 11, (const uint8_t *)p, 3);
    store_forward_push(&sf, 2, 0, 1100, 12, (const uint8_t *)p, 3);
    CHECK(store_forward_count(&sf) == 3, "buffered 3");

    /* Link up: drain oldest-first, gen_ts preserved (honest AoI). */
    sf_object_t o;
    CHECK(store_forward_pop(&sf, &o) && o.gen_ts_ms == 1000 && o.seq == 10, "pop #1 oldest, gen_ts kept");
    CHECK(store_forward_pop(&sf, &o) && o.gen_ts_ms == 1050 && o.seq == 11, "pop #2 order preserved");
    CHECK(store_forward_pop(&sf, &o) && o.gen_ts_ms == 1100, "pop #3");
    CHECK(!store_forward_pop(&sf, &o), "empty after drain");

    /* Oversized (an image) is rejected, not buffered. */
    static uint8_t big[SF_MAX_OBJECT_LEN + 1];
    CHECK(!store_forward_push(&sf, 3, 1, 2000, 1, big, sizeof(big)), "oversized rejected");
    CHECK(sf.oversized_total == 1, "oversized counted");

    /* Overflow: push CAPACITY+5; oldest 5 dropped, newest CAPACITY kept. */
    store_forward_init(&sf);
    for (uint32_t i = 0; i < SF_CAPACITY + 5U; ++i) {
        store_forward_push(&sf, 2, 0, 3000 + i, i, (const uint8_t *)p, 3);
    }
    CHECK(store_forward_count(&sf) == SF_CAPACITY, "full at capacity");
    CHECK(sf.dropped_total == 5, "5 oldest dropped");
    CHECK(store_forward_pop(&sf, &o) && o.seq == 5, "oldest kept is seq=5 (0..4 dropped)");

    printf(fails == 0 ? "\n=> store-and-forward: ALL PASS\n"
                      : "\n=> store-and-forward: %d FAIL\n", fails);
    return fails ? 1 : 0;
}
