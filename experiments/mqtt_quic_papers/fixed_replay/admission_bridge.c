#include "video_admission.h"
/* Queue-only ablation: no fabricated RTT, cwnd, in-flight, or RSSI samples.
 * Zero metrics select the C policy's documented unknown-metric fallback.
 * This does NOT reproduce the complete ESP32 transport-informed scheduler. */
static video_admission_t state;
int replay_amr_queue_allow(uint64_t now_us, uint32_t bytes, int queued) {
    return video_admission_allow(&state, 1, now_us, 0, 0, 0, 0, bytes, queued != 0);
}
