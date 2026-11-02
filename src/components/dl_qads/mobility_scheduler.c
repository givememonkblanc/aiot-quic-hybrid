#include "include/mobility_scheduler.h"
#include <string.h>
static volatile int s_mode = 2;
static volatile unsigned s_video_fps = 15, s_video_budget = 256 * 1024;
unsigned mobility_video_fps(void) { return s_video_fps; }
unsigned mobility_video_budget(void) { return s_video_budget; }
bool mobility_video_config(unsigned fps, unsigned budget_kib) {
    if (fps < 1 || fps > 30 || budget_kib < 64 || budget_kib > 6144) return false;
    s_video_budget = budget_kib * 1024; s_video_fps = fps; return true;
}
int mobility_mode_get(void) { return s_mode; }
bool mobility_mode_set(int mode) { if (mode < 0 || mode > 2) return false; s_mode = mode; return true; }
void mobility_scheduler_init(mobility_scheduler_t *s) { memset(s, 0, sizeof(*s)); s->state = MOB_OFFLINE; }
const char *mobility_state_name(mobility_state_t state)
{
    static const char *names[] = {"normal", "prepare", "offline", "validating", "recovery"};
    return state <= MOB_RECOVERY ? names[state] : "invalid";
}
static void remove_at(mobility_scheduler_t *s, unsigned i)
{
    if (i >= s->count) return;
    memmove(&s->queue[i], &s->queue[i+1], (s->count-i-1)*sizeof(s->queue[0]));
    s->count--;
}
void mobility_scheduler_update(mobility_scheduler_t *s, bool link_ready, bool validating,
                               bool path_ready, int rssi, uint32_t rtt_ms, uint64_t now)
{
    if (!link_ready) { s->state = MOB_OFFLINE; s->weak_samples = s->healthy_samples = 0; return; }
    if (validating || !path_ready) { s->state = MOB_VALIDATING; return; }
    if (s->state == MOB_OFFLINE || s->state == MOB_VALIDATING) {
        s->state = MOB_RECOVERY; s->recovery_until_us = now + 2000000;
    }
    if (s->state == MOB_RECOVERY && now < s->recovery_until_us) return;
    bool weak = (rssi < 0 && rssi < -70) || rtt_ms > 150;
    s->weak_samples = weak ? s->weak_samples + 1 : 0;
    s->healthy_samples = weak ? 0 : s->healthy_samples + 1;
    if (s->weak_samples >= 3) s->state = MOB_PREPARE;
    else if (s->healthy_samples >= 5 || s->state == MOB_RECOVERY) s->state = MOB_NORMAL;
}
bool mobility_scheduler_push(mobility_scheduler_t *s, const mobility_object_t *o, int mode)
{
    if (o->len > MOBILITY_PAYLOAD_CAPACITY) return false;
    if (s->count == MOBILITY_QUEUE_CAPACITY) {
        s->overflow++;
        if (mode == 2 && o->critical) {
            unsigned victim;
            for (victim = 0; victim < s->count && s->queue[victim].critical; ++victim) {}
            if (victim == s->count) { s->critical_overflow++; return false; }
            remove_at(s, victim);
        } else {
            if (o->critical) s->critical_overflow++;
            return false;
        }
    }
    s->queue[s->count++] = *o;
    return true;
}
int mobility_scheduler_pick(mobility_scheduler_t *s, int mode, uint64_t now)
{
    if (mode == 2) {
        for (unsigned i = 0; i < s->count;) {
            if (!s->queue[i].critical && now >= s->queue[i].generated_us &&
                now-s->queue[i].generated_us >= 500000) { remove_at(s, i); s->expired++; }
            else i++;
        }
    }
    if (s->state == MOB_OFFLINE || s->state == MOB_VALIDATING || !s->count || now < s->next_send_us) return -1;
    if (mode != 2) return 0;
    for (unsigned i = 0; i < s->count; ++i) if (s->queue[i].critical) return (int)i;
    /* Ordinary telemetry remains queued during warning; expires at 500ms. */
    if (s->state == MOB_PREPARE) return -1;
    return (int)s->count-1;
}
void mobility_scheduler_commit(mobility_scheduler_t *s, unsigned index, int mode, uint64_t now)
{
    remove_at(s, index);
    s->next_send_us = now + ((mode == 2 && s->state == MOB_RECOVERY) ? 100000 : 20000);
}

#ifdef AIOT_ESPDET_RUNTIME
/* Temporary integration build: profile stays nonzero, disabling OTA polling. */
static int s_profile = 1; /* Establish transport before allocating inference scratch. */
#else
static int s_profile;
#endif
int mobility_profile_get(void) { return __atomic_load_n(&s_profile, __ATOMIC_RELAXED); }
bool mobility_profile_set(int p) { if (p < 0 || p > 8) return false; __atomic_store_n(&s_profile, p, __ATOMIC_RELAXED); return true; }

static unsigned s_probe_hz;
unsigned mobility_probe_hz(void) { return __atomic_load_n(&s_probe_hz, __ATOMIC_RELAXED); }
bool mobility_probe_set(unsigned hz) { if (hz > 10) return false; __atomic_store_n(&s_probe_hz, hz, __ATOMIC_RELAXED); return true; }

static unsigned s_ai_priority, s_replay_epoch, s_replay_generation;
unsigned mobility_ai_priority(void) { return __atomic_load_n(&s_ai_priority, __ATOMIC_RELAXED); }
bool mobility_ai_priority_set(unsigned p) { if (p > 6) return false; __atomic_store_n(&s_ai_priority,p,__ATOMIC_RELAXED);return true; }
unsigned mobility_replay_epoch(void) { return __atomic_load_n(&s_replay_epoch,__ATOMIC_RELAXED); }
bool mobility_replay_set(unsigned enabled) { if (enabled > 1) return false; __atomic_store_n(&s_replay_epoch,enabled ? ++s_replay_generation : 0,__ATOMIC_RELAXED);return true; }
