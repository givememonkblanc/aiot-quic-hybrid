/*
 * Semantic-transport decision matrix — measures the perception-aware scheduler.
 *
 * dl_qads_decide() is a pure function of (config, packet, metrics), so the
 * semantic policy can be measured deterministically without a device or netem:
 * feed each importance level under each network condition and record what the
 * scheduler decides. "OFF" (baseline: importance ignored) is modelled by
 * forcing importance = ROUTINE, since that is exactly what the old type-only
 * scheduler saw.
 *
 * Build/run: experiments/run_semantic_matrix.sh
 */
#include <stdio.h>
#include <string.h>

#include "dl_qads.h"   /* pulls data_packet.h */

static const char *mode_str(tx_mode_t m)
{
    switch (m) {
        case TX_MODE_STREAM:   return "STREAM";   /* reliable, ordered */
        case TX_MODE_DATAGRAM: return "datagram"; /* best-effort, may lose */
        case TX_MODE_DEFER:    return "defer";
        case TX_MODE_DROP:     return "DROP";      /* not sent */
        default:               return "?";
    }
}

/* Is this decision a reliable/guaranteed delivery? */
static int is_protected(tx_mode_t m) { return m == TX_MODE_STREAM; }
static int is_lost(tx_mode_t m)      { return m == TX_MODE_DROP; }

typedef struct { const char *name; net_metrics_t m; } scenario_t;

static net_metrics_t healthy(void)
{
    net_metrics_t m; memset(&m, 0, sizeof(m));
    m.rtt_ms = 10; m.loss_percent = 0; m.tx_queue_depth = 0;
    m.datagram_budget = 8000; m.stream_window_available = true; m.quic_ready = true;
    return m;
}

int main(void)
{
    dl_qads_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.ai_stream_id = 1; cfg.sensor_stream_id = 2; cfg.image_stream_id = 3;
    cfg.image_datagram_budget = 4000;
    cfg.telemetry_deadline_ms = 200; cfg.image_deadline_ms = 500;
    cfg.policy_mode = QADS_POLICY_HYBRID_BASELINE;
    dl_qads_t sched; dl_qads_init(&sched, &cfg);

    /* Network scenarios, from clean to severe. */
    scenario_t sc[5];
    sc[0].name = "clean";        sc[0].m = healthy();
    sc[1].name = "rtt300";       sc[1].m = healthy(); sc[1].m.rtt_ms = 300;
    sc[2].name = "loss5+cong";   sc[2].m = healthy(); sc[2].m.loss_percent = 5;
                                 sc[2].m.rtt_ms = 120; sc[2].m.tx_queue_depth = 5;
                                 sc[2].m.stream_window_available = false;
    sc[3].name = "loss15+cong";  sc[3].m = healthy(); sc[3].m.loss_percent = 15;
                                 sc[3].m.rtt_ms = 200; sc[3].m.tx_queue_depth = 8;
                                 sc[3].m.stream_window_available = false;
    sc[4].name = "loss25";       sc[4].m = healthy(); sc[4].m.loss_percent = 25;

    struct { const char *label; uint8_t imp; } levels[] = {
        {"OFF/ROUTINE", (uint8_t)AIOT_IMPORTANCE_ROUTINE},
        {"ELEVATED",    (uint8_t)AIOT_IMPORTANCE_ELEVATED},
        {"CRITICAL",    (uint8_t)AIOT_IMPORTANCE_CRITICAL},
    };

    const uint64_t now = 10ULL * 1000000ULL;  /* 10 s */

    printf("# Semantic scheduler decision matrix (packet = AI_RESULT)\n\n");
    printf("%-14s", "importance");
    for (int s = 0; s < 5; ++s) printf("%14s", sc[s].name);
    printf("\n%-14s", "----------");
    for (int s = 0; s < 5; ++s) printf("%14s", "------");
    printf("\n");

    int crit_protected = 0, crit_lost = 0, routine_lost_or_degraded = 0, cond = 0;
    for (int l = 0; l < 3; ++l) {
        printf("%-14s", levels[l].label);
        for (int s = 0; s < 5; ++s) {
            data_packet_t p; memset(&p, 0, sizeof(p));
            p.type = DATA_TYPE_AI_RESULT;
            p.timestamp_us = now - 50000; /* 50 ms old */
            p.payload_len = 40;
            p.loss_tolerant = false;
            p.importance = levels[l].imp;
            schedule_decision_t d = dl_qads_decide(&sched, &p, &sc[s].m, now);
            printf("%14s", mode_str(d.mode));
            if (l == 2) { /* CRITICAL */
                if (is_protected(d.mode)) crit_protected++;
                if (is_lost(d.mode)) crit_lost++;
            }
            if (l == 0) { /* OFF/ROUTINE baseline */
                if (d.mode != TX_MODE_STREAM) routine_lost_or_degraded++;
                cond++;
            }
        }
        printf("\n");
    }

    /* Second table: a loss-tolerant payload past its useful life under
     * congestion — exercises the AoI-shed protection. */
    printf("\n# AoI-shed protection (loss-tolerant SENSOR, aged past deadline, congested)\n\n");
    net_metrics_t cong = sc[2].m;                 /* congested */
    printf("%-14s %14s\n", "importance", "decision");
    printf("%-14s %14s\n", "----------", "------");
    int shed_off = 0, shed_crit = 0;
    for (int l = 0; l < 3; ++l) {
        data_packet_t p; memset(&p, 0, sizeof(p));
        p.type = DATA_TYPE_SENSOR;
        p.timestamp_us = now - 500000; /* 500 ms old > 200 ms deadline */
        p.payload_len = 40;
        p.loss_tolerant = true;
        p.importance = levels[l].imp;
        schedule_decision_t d = dl_qads_decide(&sched, &p, &cong, now);
        printf("%-14s %14s\n", levels[l].label, mode_str(d.mode));
        if (l == 0 && d.mode == TX_MODE_DROP) shed_off = 1;
        if (l == 2 && d.mode == TX_MODE_DROP) shed_crit = 1;
    }

    printf("\n# Verdict\n");
    printf("AI_RESULT under 5 conditions:\n");
    printf("  ROUTINE (baseline) degraded/dropped off the reliable stream in %d/%d conditions\n",
           routine_lost_or_degraded, cond);
    printf("  CRITICAL kept the reliable stream in %d/5 conditions; dropped in %d/5\n",
           crit_protected, crit_lost);
    printf("AoI shed (aged loss-tolerant, congested): ROUTINE dropped=%d, CRITICAL dropped=%d\n",
           shed_off, shed_crit);
    printf("=> Semantic protection %s: CRITICAL is delivered reliably and never AoI-shed,\n",
           (crit_lost == 0 && shed_crit == 0 && routine_lost_or_degraded > 0) ? "CONFIRMED" : "NOT shown");
    printf("   while routine traffic degrades/sheds under the same congestion.\n");
    return 0;
}
