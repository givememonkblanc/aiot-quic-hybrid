#include "video_admission.h"
#include <string.h>
static unsigned selected;
bool video_policy_set(unsigned policy) {
    if (policy > 3) return false;
    __atomic_store_n(&selected, policy, __ATOMIC_RELAXED); return true;
}
unsigned video_policy_get(void) { return __atomic_load_n(&selected, __ATOMIC_RELAXED); }
bool video_admission_allow(video_admission_t *s, unsigned policy, uint64_t now,
                           int rssi, uint32_t rtt, uint32_t cwnd,
                           uint32_t inflight, uint32_t bytes, bool queued) {
    if (policy > 3) return false;
    if (s->policy != (int)policy) { memset(s, 0, sizeof(*s)); s->policy = policy; }
    /* AMR uses 500ms RSSI sampling. Unknown RSSI never means BAD or GOOD. */
    if (!s->sample_us || now - s->sample_us >= 500000) {
        s->sample_us = now;
        bool known = rssi < 0 && rssi >= -127;
        if (known) s->rssi_ewma = s->rssi_ewma ? (45*rssi+55*s->rssi_ewma)/100 : rssi;
        if (rtt) s->rtt_ewma = s->rtt_ewma ? (45ULL*rtt+55ULL*s->rtt_ewma)/100 : rtt;
        bool bad = known && s->rssi_ewma <= -75;
        bool good = known && s->rssi_ewma > -67;
        if (policy == 3) {
            bad = bad || (rtt && s->rtt_ewma > 180);
            good = good && rtt && s->rtt_ewma <= 150;
        }
        if (bad) { s->weak = true; s->good_since_us = 0; }
        else if (good) {
            if (!s->good_since_us) s->good_since_us = now;
            if (now - s->good_since_us >= 1000000) s->weak = false;
        } else s->good_since_us = 0;
    }
    if (queued) return false; /* AMR latest-frame / per-media empty queue rule. */
    if (policy == 0) {
        if (cwnd <= inflight || cwnd - inflight < 1064) return false;
        if ((uint64_t)bytes*rtt/cwnd+rtt > 1000 &&
            s->last_admit_us && now-s->last_admit_us < 1000000) return false;
    } else {
        /* AMR BW*200ms backstop. cwnd/RTT is only a rate proxy here;
         * ngtcp2 exposes no delivery-rate estimate in the current adapter. */
        uint64_t cap = cwnd && rtt ? (uint64_t)cwnd*200/rtt : 131072;
        if (cap < 65536) cap = 65536;
        if (inflight > cap) return false;
        /* No second path: throttle weak-link video, retain one probe/sec.
         * This does not initiate AP changes or pretend a backup exists. */
        if (policy >= 2 && s->weak && s->last_admit_us &&
            now-s->last_admit_us < 1000000) return false;
    }
    s->last_admit_us = now;
    return true;
}
