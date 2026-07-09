#include "include/presence.h"

#include <stdio.h>
#include <string.h>

static void presence_transition(presence_t *p, presence_state_t next,
                                uint8_t score, int64_t now_us,
                                presence_event_t *out_event)
{
    int64_t prev_dur_us = (p->state_since_us > 0) ? (now_us - p->state_since_us) : 0;

    p->event_seq++;
    if (out_event != NULL) {
        out_event->state = next;
        out_event->event_seq = p->event_seq;
        out_event->prev_state_dur_ms = (uint32_t)(prev_dur_us / 1000);
        out_event->score = score;
    }
    p->state = next;
    p->state_since_us = now_us;
    p->consecutive_positive = 0;
}

void presence_init(presence_t *p, const presence_config_t *config)
{
    memset(p, 0, sizeof(*p));
    if (config != NULL) {
        p->config = *config;
    } else {
        p->config = (presence_config_t)PRESENCE_CONFIG_DEFAULTS;
    }
    if (p->config.enter_frames == 0) {
        p->config.enter_frames = 1;
    }
    p->state = PRESENCE_STATE_UNKNOWN;
}

uint8_t presence_score(const aiot_ai_result_t *result)
{
    uint8_t score = 0;
    if (result == NULL) {
        return 0;
    }
    for (uint8_t i = 0; i < result->detection_count && i < AIOT_AI_RESULT_MAX_DETECTIONS; i++) {
        if (result->detections[i].confidence > score) {
            score = result->detections[i].confidence;
        }
    }
    return score;
}

bool presence_update(presence_t *p, const aiot_ai_result_t *result,
                     int64_t now_us, presence_event_t *out_event)
{
    uint8_t score = presence_score(result);

    p->frames_total++;
    p->last_score = score;
    if (p->state_since_us == 0) {
        p->state_since_us = now_us;
    }

    bool keeps_alive = (p->state == PRESENCE_STATE_PRESENT)
        ? (score >= p->config.exit_confidence)
        : (score >= p->config.enter_confidence);

    if (keeps_alive) {
        p->frames_positive++;
        p->last_positive_us = now_us;
    }

    switch (p->state) {
    case PRESENCE_STATE_PRESENT:
        break; /* exit handled by the timeout below */
    case PRESENCE_STATE_UNKNOWN:
    case PRESENCE_STATE_ABSENT:
        if (score >= p->config.enter_confidence) {
            if (++p->consecutive_positive >= p->config.enter_frames) {
                presence_transition(p, PRESENCE_STATE_PRESENT, score, now_us, out_event);
                return true;
            }
        } else {
            p->consecutive_positive = 0;
            if (p->state == PRESENCE_STATE_UNKNOWN) {
                presence_transition(p, PRESENCE_STATE_ABSENT, score, now_us, out_event);
                return true;
            }
        }
        break;
    }

    return presence_tick(p, now_us, out_event);
}

bool presence_tick(presence_t *p, int64_t now_us, presence_event_t *out_event)
{
    if (p->state != PRESENCE_STATE_PRESENT) {
        return false;
    }
    int64_t silence_us = now_us - p->last_positive_us;
    if (p->last_positive_us > 0 &&
        silence_us >= (int64_t)p->config.exit_timeout_ms * 1000) {
        presence_transition(p, PRESENCE_STATE_ABSENT, p->last_score, now_us, out_event);
        return true;
    }
    return false;
}

const char *presence_state_name(presence_state_t state)
{
    switch (state) {
    case PRESENCE_STATE_PRESENT: return "present";
    case PRESENCE_STATE_ABSENT: return "absent";
    default: return "unknown";
    }
}

int presence_event_json(const presence_event_t *event, const char *device_id,
                        int64_t now_us, char *buf, size_t cap)
{
    int written = snprintf(buf, cap,
        "{\"device\":\"%s\",\"seq\":%lu,\"state\":\"%s\",\"score\":%u,"
        "\"prev_state_dur_ms\":%lu,\"ts_ms\":%lld}",
        device_id != NULL ? device_id : "",
        (unsigned long)event->event_seq,
        presence_state_name(event->state),
        (unsigned)event->score,
        (unsigned long)event->prev_state_dur_ms,
        (long long)(now_us / 1000));
    return (written < 0 || (size_t)written >= cap) ? -1 : written;
}

int presence_heartbeat_json(const presence_t *p, const char *device_id,
                            int64_t now_us, int wifi_rssi_dbm,
                            uint32_t heap_free, char *buf, size_t cap)
{
    uint32_t state_dur_ms = (p->state_since_us > 0)
        ? (uint32_t)((now_us - p->state_since_us) / 1000) : 0;
    int written = snprintf(buf, cap,
        "{\"device\":\"%s\",\"state\":\"%s\",\"score\":%u,\"state_dur_ms\":%lu,"
        "\"frames\":%lu,\"positive\":%lu,\"rssi\":%d,\"heap\":%lu,\"ts_ms\":%lld}",
        device_id != NULL ? device_id : "",
        presence_state_name(p->state),
        (unsigned)p->last_score,
        (unsigned long)state_dur_ms,
        (unsigned long)p->frames_total,
        (unsigned long)p->frames_positive,
        wifi_rssi_dbm,
        (unsigned long)heap_free,
        (long long)(now_us / 1000));
    return (written < 0 || (size_t)written >= cap) ? -1 : written;
}
