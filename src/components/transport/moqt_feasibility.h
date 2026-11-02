#ifndef AIOT_MOQT_FEASIBILITY_H
#define AIOT_MOQT_FEASIBILITY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Conservative application service rate, NOT a path-capacity estimator.
 * Includes scheduling/ACK latency. No knowledge of the shaper is used. */
typedef struct { uint64_t bytes_per_second; unsigned samples; uint64_t deviation; } moqt_rate;
static inline void moqt_rate_observe(moqt_rate *r, size_t acked, uint64_t elapsed_us) {
    if (!elapsed_us) return;
    uint64_t sample = (uint64_t)acked * 1000000 / elapsed_us;
    if (sample < 1000) sample = 1000;
    if (sample > 4000000) sample = 4000000;
    /* React quickly to degradation; increase cautiously. */
    r->bytes_per_second = !r->samples || sample < r->bytes_per_second ? sample :
        (3 * r->bytes_per_second + sample) / 4;
    r->samples++;
}
/* Samples of short windows scatter around the rate: both directions are smoothed, the
 * decrease less than the increase. */
static inline void moqt_rate_observe_window(moqt_rate *r, size_t acked, uint64_t elapsed_us) {
    if (!elapsed_us) return;
    uint64_t sample = (uint64_t)acked * 1000000 / elapsed_us;
    if (sample < 1000) sample = 1000;
    if (sample > 4000000) sample = 4000000;
    if (!r->samples) {
        r->bytes_per_second = sample;
        r->deviation = sample / 4;   /* one sample says little about the scatter */
    } else {
        uint64_t off = sample > r->bytes_per_second ? sample - r->bytes_per_second : r->bytes_per_second - sample;
        r->deviation = (3 * r->deviation + off) / 4;
        if (sample < r->bytes_per_second) r->bytes_per_second = (r->bytes_per_second + sample) / 2;
        else r->bytes_per_second = (3 * r->bytes_per_second + sample) / 4;
    }
    r->samples++;
}
/* Mean deviation of the samples; half the default rate while there is no sample. */
static inline uint64_t moqt_rate_deviation(const moqt_rate *r) {
    return r->samples ? r->deviation : 32000;
}
static inline uint64_t moqt_rate_value(const moqt_rate *r) {
    return r->samples ? r->bytes_per_second : 64000;
}
static inline bool moqt_fits(size_t bytes, size_t queued, uint64_t bps,
                              uint64_t now, uint64_t deadline) {
    if (!bps || deadline <= now || bytes > 262144 || queued > 1048576) return false;
    /* 25% service margin + 100 ms guard. Sizes bounded above avoid overflow. */
    uint64_t service = ((uint64_t)bytes + queued + 32) * 1250000 / bps;
    return deadline - now >= service + 100000;
}
/* Representations ordered from highest utility to lowest, not by size. */
static inline int moqt_choose_representation(const size_t *sizes, unsigned count,
        uint64_t bps, uint64_t now, uint64_t deadline) {
    for (unsigned i = 0; i < count; ++i)
        if (sizes[i] && moqt_fits(sizes[i], 0, bps, now, deadline)) return (int)i;
    return -1;
}
#endif
