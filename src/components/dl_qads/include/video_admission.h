#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Single-path adaptations, not multipath selector implementations. */
typedef struct {
    int policy;
    bool weak;
    int rssi_ewma;
    uint32_t rtt_ewma;
    uint64_t sample_us, good_since_us, last_admit_us;
} video_admission_t;
bool video_policy_set(unsigned policy);
unsigned video_policy_get(void);
bool video_admission_allow(video_admission_t *s, unsigned policy, uint64_t now,
                           int rssi, uint32_t rtt_ms, uint32_t cwnd,
                           uint32_t inflight, uint32_t bytes, bool queued);
