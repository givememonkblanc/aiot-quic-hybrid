/* Execute the imported selectors with controlled path metrics. This is a
 * decision probe, not a transport simulation or a throughput benchmark. */
#include "reference/src/client/scheduler/sched_pqi.h"
#include <assert.h>

/* No external logging side effects; all scheduling and health code is original. */
void qlog_switch_event(qlog_t *q, uint64_t t, int from, int to, const char *why) {
    (void)q; (void)t; (void)from; (void)to; (void)why;
}

int main(void) {
    const char *names[] = {"pqi", "rssi", "minrtt", "spquic", "rr", "ecf", "blest", "tof"};
    const char *cases[] = {"healthy", "rtt_loss_degraded", "verified_but_dead", "unknown_rssi"};
    const uint64_t now = 10000000;
    puts("scenario,policy,path,reason");
    for (int scenario = 0; scenario < 4; ++scenario) {
        for (int mode = 0; mode < 8; ++mode) {
            tx_t st = {0};
            picoquic_cnx_t c = {0};
            picoquic_path_t p[2] = {0};
            picoquic_path_t *paths[] = {&p[0], &p[1]};
            picoquic_tuple_t tuples[2] = {0};
            c.path = paths; c.nb_paths = 2;
            st.scheduler_mode = mode;
            st.pqi_last_choice = 0;
            st.pqi_last_switch_us = st.pqi_choice_since_us = 1;
            st.wifi_last_rssi_dbm = -55;
            st.ip_primary_be = inet_addr("192.168.1.2");
            for (int i = 0; i < 2; ++i) {
                p[i].first_tuple = &tuples[i];
                tuples[i].challenge_verified = 1;
                ((struct sockaddr_in *)&tuples[i].local_addr)->sin_addr.s_addr =
                    inet_addr(i ? "10.0.0.2" : "192.168.1.2");
                p[i].smoothed_rtt = i ? 80000 : 20000;
                st.pqi_rtt_ewma[i] = p[i].smoothed_rtt;
                st.pqi_bw_ewma[i] = 1000000;
                st.path_rssi_ewma_dbm[i] = i ? INT_MIN : -55;
                st.path_last_progress_us[i] = now - 1000;
            }
            if (scenario == 1) {
                p[0].smoothed_rtt = st.pqi_rtt_ewma[0] = 400000;
                st.pqi_grade[0] = 1; st.pqi_loss_bp_ewma[0] = 1000;
            } else if (scenario == 2) {
                p[0].nb_retransmit = 3; p[0].bytes_in_transit = 1200;
                st.pqi_grade[0] = 2;
            } else if (scenario == 3) {
                st.path_rssi_ewma_dbm[0] = st.wifi_last_rssi_dbm = INT_MIN;
            }
            sched_choice_t choice = sched_dispatch(&st, &c, 0, 1, now);
            printf("%s,%s,%d,%s\n", cases[scenario], names[mode], choice.use_i, choice.reason);
            if (mode >= 5) assert(choice.use_i == 0 && strstr(choice.reason, "TODO_"));
            if (scenario == 1 && mode == 0) assert(choice.use_i == 1);
            if (scenario == 2 && mode == 2) assert(choice.use_i == 0);
            if (scenario == 2 && mode == 3) assert(choice.use_i == 1);
            if (scenario == 3 && mode == 1) assert(choice.use_i == 1);
            if (scenario == 0 && mode == 1) {
                st.path_rssi_ewma_dbm[0] = -80;
                choice = sched_dispatch(&st, &c, 0, 1, now);
                assert(choice.use_i == 1 && st.pqi_last_choice == 0);
                st.path_rssi_ewma_dbm[0] = -70;
                choice = sched_dispatch(&st, &c, 0, 1, now + 1000);
                assert(choice.use_i == 0); /* Caller never commits RSSI choice. */
                printf("rssi_uncommitted_return,rssi,%d,%s\n", choice.use_i, choice.reason);
                st.pqi_last_choice = 1; st.path_rssi_ewma_dbm[0] = -55;
                choice = sched_dispatch(&st, &c, 0, 1, now + 2000);
                assert(choice.use_i == 0); /* Backup tenure, not GOOD duration. */
                printf("first_good_after_long_backup,rssi,%d,%s\n", choice.use_i, choice.reason);
            }
        }
    }
    return 0;
}
