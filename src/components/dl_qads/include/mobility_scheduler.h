#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MOBILITY_QUEUE_CAPACITY 64
#define MOBILITY_PAYLOAD_CAPACITY 160

typedef enum { MOB_NORMAL, MOB_PREPARE, MOB_OFFLINE, MOB_VALIDATING, MOB_RECOVERY } mobility_state_t;
typedef struct {
    uint32_t seq;
    uint64_t generated_us;
    bool critical;
    uint16_t len;
    char payload[MOBILITY_PAYLOAD_CAPACITY];
} mobility_object_t;
typedef struct {
    mobility_state_t state;
    mobility_object_t queue[MOBILITY_QUEUE_CAPACITY];
    unsigned count, weak_samples, healthy_samples;
    uint64_t recovery_until_us, next_send_us;
    uint32_t expired, overflow, critical_overflow;
} mobility_scheduler_t;
void mobility_scheduler_init(mobility_scheduler_t *s);
void mobility_scheduler_update(mobility_scheduler_t *s, bool link_ready, bool validating,
                               bool path_ready, int rssi, uint32_t rtt_ms, uint64_t now);
bool mobility_scheduler_push(mobility_scheduler_t *s, const mobility_object_t *object, int mode);
/* Peek until the caller accepts the object; commit only on successful enqueue. */
int mobility_scheduler_pick(mobility_scheduler_t *s, int mode, uint64_t now);
void mobility_scheduler_commit(mobility_scheduler_t *s, unsigned index, int mode, uint64_t now);
const char *mobility_state_name(mobility_state_t state);
/* Runtime mode: 0 reconnect/FIFO; 1 migration/FIFO; 2 migration/adaptive. */
int mobility_mode_get(void);
bool mobility_mode_set(int mode);
bool mobility_video_config(unsigned fps, unsigned budget_kib);
unsigned mobility_video_fps(void);
unsigned mobility_video_budget(void);

/* 0 legacy, 1 camera, 2 local AI, 3 metadata, 4 evidence, 5 preview. */
int mobility_profile_get(void);
bool mobility_profile_set(int profile);

unsigned mobility_probe_hz(void);
bool mobility_probe_set(unsigned hz);

unsigned mobility_ai_priority(void);
bool mobility_ai_priority_set(unsigned priority);
unsigned mobility_replay_epoch(void);
bool mobility_replay_set(unsigned enabled);
