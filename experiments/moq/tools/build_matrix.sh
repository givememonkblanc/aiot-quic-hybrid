#!/usr/bin/env bash
# Images of the method matrix (campaign_matrix.sh): every method twice, under the names mx-a-<method>
# and mx-b-<method>, so that a run always installs an image other than the one running. All images
# share the screw model, the binary result, the AI core, a run of 213 s (three rotations of the
# turntable), and the mission options of their base; they differ in the publisher policy only.
#   fifo     priority base with the results at the priority of the images (128): no precedence
#   prio     results first, medium representation, no image timeout       (gurel2024prio)
#   prio-to  prio and an image timeout of D = 10 s                        (freeman2025pc); also IMQ
#   abr      throughput-based representation, prio-to                     (FESTIVE)
#   dtp      deadline-aware: the largest representation that meets its deadline at 0.8 of the
#            estimated rate, prio-to                                      (DTP)
#   bbr      prio-to over BBR congestion control instead of CUBIC         (BBR)
#   prio-to-tau      prio-to with the deadlines of the images at the mission budget: a baseline
#                    that knows the budget. On the device D = 1200 ms from the capture (the
#                    inference, about 1.0 s, plus tau = 200 ms; 200 ms from the capture would expire
#                    every image); at the relay D = 200 ms
#   prio-to-low      prio-to at the low representation: a baseline that sends small images
#   prio-to-low-tau  both: small images and the deadlines of prio-to-tau
#                    (the images reach the send queue 3.8-7 s after the capture, after the passage is
#                    confirmed, so D = 1.2 s expires every image: prio-to-tau sends the results only)
#   prio-to-low-d5   the strongest tuned baseline: small images and D = 5 s, the shortest deadline
#                    that lets the images through (median 4.5 s from the capture to the send queue)
#   mqtt     prio publisher over MQTT 3.1.1 with TLS (moq_mqtt_shim.c)
#   mbm      mission-based: image window, representation plan, result probe of 50 ms
#   mbm-floor    mbm whose plan values every representation alike (the mission needs an image
#                at the low representation or above, 400x300, and nothing more): it covers the
#                most passages with the fewest bytes
#   mbm-floor-fix16k, mbm-floor-fix1k6  ablation: mbm-floor with a constant evidence window of
#                16384 bytes (TCP_NOTSENT_LOWAT as Cloudflare sets it for HTTP/2 priorities) or 1600
#                bytes (the derived window at 128 kbit/s) in place of the derived one; the relay
#                takes the same constant (relay_receiver.py --fixed-window)
#   mbm-floor-bdp  mbm-floor whose evidence window also holds the bytes on the wire (rate x
#                smallest round trip; AIOT_MOQT_MUS_WINDOW_BDP, relay --window-bdp): without them a
#                round trip longer than the delay caps the evidence at window/rtt
#   implicit-<T>     base reproduction (Freeman et al., latency-driven implicit adaptation with MoQ):
#                    results first, small images, every Object (results too) with the delivery
#                    timeout T from its creation, reset when it expires; relay --timeout-all
#   mbm-floor-t30    mbm-floor with a device budget of 30 ms: the share of the alerting device in a
#                    shared uplink (stage 0, theory grant of neighbor_video.py)
#   mbm-floor-pred, mbm-pred  with AIOT_MOQT_MUS_PREDICT: image bytes that drain before the next
#                    inference can end are admitted beyond the window (predictive MBM)
#   mbm-floor-wmm, prio-to-low-wmm  with AIOT_MOQT_WMM: result, control and acknowledgement packets
#                go in the Wi-Fi voice access category, image packets best effort
#   mbm-noprobe  ablation, mbm without the result probe (one name only)
# usage: build_matrix.sh [method...]   (default: all)
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
# MODEL=model-c3 TAG=-c3: the images of another model under names ending in TAG (10/7); EXTRA: more options
M=/home/ryzen395/data/aiot_quic_results/screw_turntable/${MODEL:-model-f1}
common="-DAIOT_PROFILE_AI_CORE=1 -DAIOT_PROFILE_AI_PRIORITY=1 -DAIOT_MOQT_RESULT_BINARY=ON -DAIOT_MOQT_RUN_SECONDS=213 $EXTRA"
spec() {
  case $1 in
    fifo) echo "prio-med -DAIOT_MOQT_MISSION_RESULT_PRIORITY=128 -DAIOT_MOQT_RESULT_PROBE_MS=0";;
    prio) echo "prio-med -DAIOT_MOQT_RESULT_PROBE_MS=0";;
    prio-to) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0";;
    abr) echo "abr -DAIOT_MOQT_MISSION_IMAGE_TIMEOUT=ON -DAIOT_MOQT_RESULT_PROBE_MS=0";;
    dtp) echo "feasible -DAIOT_MOQT_MISSION_IMAGE_TIMEOUT=ON -DAIOT_MOQT_RESULT_PROBE_MS=0";;
    bbr) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_QUIC_CC=bbr";;
    prio-to-tau) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_AI_DEADLINE_MS=1200";;
    prio-to-low) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_REPRESENTATION=1";;
    prio-to-low-tau) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_REPRESENTATION=1 -DAIOT_MOQT_AI_DEADLINE_MS=1200";;
    prio-to-low-d5) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_REPRESENTATION=1 -DAIOT_MOQT_AI_DEADLINE_MS=5000";;
    mqtt) echo "prio-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MQTT_BASELINE=ON -DAIOT_MQTT_CA=$PWD/vendor/nbcert/cert.pem";;
    mbm) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50";;
    mbm-floor) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000";;
    mbm-floor-fix16k) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_MUS_FIXED_WINDOW=16384";;
    mbm-floor-fix1k6) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_MUS_FIXED_WINDOW=1600";;
    mbm-floor-bdp) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_MUS_WINDOW_BDP=ON";;
    mbm-off) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    prio-off) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_REPRESENTATION=1 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-t5) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=5000 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off2) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_MODE=1 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-e) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_EIGHTH=85 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off2-e) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_MODE=1 -DAIOT_MOQT_OFFLOAD_EIGHTH=85 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off2-s) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_MODE=1 -DAIOT_MOQT_OFFLOAD_SCHED=1 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off2-s2) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_MODE=1 -DAIOT_MOQT_OFFLOAD_SCHED=2 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-s) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_SCHED=1 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-p) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-sy) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-sy-s) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_OFFLOAD_SCHED=1 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-ar) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-ar-s) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_SCHED=1 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-ch) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-ch-s) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_SCHED=1 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    # 10/12: direction task per counted passage, T_max 2 s from the passage (OFFLOAD_TASK_MS), done
    # at the helper (inference 0.45 s measured: OFFLOAD_MS 500); the rule decides when the frame goes:
    # mbm-offt-all every task, -ch Chen (T_max feasibility), -ar Argus (helper ends first). Transport:
    # FIFO send queue without expiry, or -m the MoQ priorities with the object expiring at T_max.
    mbm-offt-all) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON";;
    mbm-offt-all-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000";;
    mbm-offt-ch) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON";;
    mbm-offt-ch-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000";;
    mbm-offt-ar) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON";;
    mbm-offt-ar-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000";;
    # 10/12: Chen view, T_max 4 s (UXGA 53 kB at the camera's ~50 kB/s: >=1.0 s, helper 0.45 s): the task and
    # the Chen feasibility at 4 s; -m expires the offload object at T_max (4 s)
    mbm-offt4-all) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=4000 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON";;
    mbm-offt4-all-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=4000 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000";;
    mbm-offt4-ch) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=4000 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON";;
    mbm-offt4-ch-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=4000 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000";;
    mbm-offt4-ar) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=4000 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON";;
    mbm-offt4-ar-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=4000 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000";;
    # 10/12 throughput campaign (Chen view): the camera's frames to the helper under the Chen rule
    # (pipelined, the next frame when the previous transmission ended and one helper inference, 0.45 s
    # measured: OFFLOAD_MS 500, has passed; feasible if size/R + 500 ms <= T_max = 4x base = 4000 ms;
    # rate measured again after 10 s without a frame). -f one FIFO send queue without expiry
    # (Synergy); -m the MoQ priorities with the offload object expiring at T_max (the relay's order
    # decides: campaign -m offload first, -mp mission order).
    mbm-offc-f) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON";;
    mbm-offc-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000";;
    # 10/12 MQTT baseline of the input offload (UXGA frames to the helper under the Chen rule), as mqtt-offs
    mqtt-offc) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000 -DAIOT_MQTT_BASELINE=ON -DAIOT_MQTT_CA=$PWD/vendor/nbcert/cert.pem";;
    # 10/12 split inference (Chen's model partitioning): the camera decodes, the helper runs the model on
    # the raster at JPEG 85 (~2.4 kB); helper stage 400 ms (model on the small frame); -f FIFO, -m MoQ
    mbm-offs-f) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=400 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mbm-offs-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=400 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000 -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    # 10/12 split inference at the helper's measured stage (model on the small frame 307 ms: OFFLOAD_MS 310), so that
    # the camera splits nearly every frame (the decode, 0.40 s, becomes the slowest stage)
    mbm-offs2-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=310 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000 -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    # 10/13 the same with the helper frame expiring 1 s after its publication (T_max 1.5x base from the capture,
    # the camera's decode 0.4 s before): a late frame is dropped, the next one goes
    mbm-offs2-m1) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=310 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=1000 -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    # 10/13 the general offload rule (AIOT_MOQT_OFFLOAD_GENERAL): a frame goes to the helper only if it can end by
    # its capture + T_max (2 s, 2x base), the queueing delay is below 150 ms and the camera detected something in
    # the last 3 s; dropped in flight at capture + T_max - helper service. Split (offs2-g) and input offload (offc-g)
    mbm-offs2-g) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=310 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000 -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85 -DAIOT_MOQT_OFFLOAD_GENERAL=ON -DAIOT_MOQT_GEN_QDELAY_MS=150 -DAIOT_MOQT_GEN_ACTIVE_MS=3000";;
    mbm-offc-g) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000 -DAIOT_MOQT_OFFLOAD_GENERAL=ON -DAIOT_MOQT_GEN_QDELAY_MS=150 -DAIOT_MOQT_GEN_ACTIVE_MS=3000";;
    # 10/13 the general rule with the mission's share as the evidence slack (GEN_QDELAY_MS 0): a frame goes only if
    # every open event can still get its evidence by its deadline after it, in deadline order at the measured rate
    mbm-offs2-g2) echo "$(spec mbm-offs2-g | sed 's/GEN_QDELAY_MS=150/GEN_QDELAY_MS=0/')";;
    mbm-offc-g2) echo "$(spec mbm-offc-g | sed 's/GEN_QDELAY_MS=150/GEN_QDELAY_MS=0/')";;
    # 10/13 + novelty: an input frame goes only if captured in the middle third between the camera's frames
    mbm-offc-g3) echo "$(spec mbm-offc-g2) -DAIOT_MOQT_GEN_NOVELTY=ON";;
    # 10/13 + recall: the offload under way is stopped when its bytes left would make an open event miss its evidence
    mbm-offc-g4) echo "$(spec mbm-offc-g3) -DAIOT_MOQT_GEN_RECALL=ON";;
    # 10/13 classic schedulers on the same MoQ sender (moqt_scheduler.c), split at 310 ms: strict priority to the results
    # (MOQT_AI_FIRST), deficit round robin (MOQT_DRR), earliest deadline first with the 4 s offload deadline (MOQT_EDF)
    mbm-offs2-sp) echo "$(spec mbm-offs2-f) -DAIOT_MOQT_TX_FIFO=OFF -DAIOT_MOQT_POLICY=1";;
    mbm-offs2-drr) echo "$(spec mbm-offs2-f) -DAIOT_MOQT_TX_FIFO=OFF -DAIOT_MOQT_POLICY=2";;
    mbm-offs2-edf) echo "$(spec mbm-offs2-m) -DAIOT_MOQT_POLICY=3";;
    # 10/13 the same three on the wire send path of FIFO and MBM (AIOT_MOQT_TX_ORDER: same slices and window, only the
    # choice of the next stream differs): strict priority, round robin, EDF with the 4 s offload deadline
    mbm-offs2-sp2) echo "$(spec mbm-offs2-f) -DAIOT_MOQT_TX_FIFO=OFF -DAIOT_MOQT_TX_ORDER=1";;
    mbm-offs2-rr2) echo "$(spec mbm-offs2-f) -DAIOT_MOQT_TX_FIFO=OFF -DAIOT_MOQT_TX_ORDER=2";;
    mbm-offs2-edf2) echo "$(spec mbm-offs2-m) -DAIOT_MOQT_TX_ORDER=3";;
    # 10/13 structure A: the helper's frames decoded on core 0 by their own task, the detector on core 1 infers every frame
    mbm-offs2-c0) echo "$(spec mbm-offs2-m1) -DAIOT_MOQT_SPLIT_CORE0=ON";;
    # 10/13 structure A with prefetch: the next raster is decoded on core 0 while the helper infers the previous one
    mbm-offs2-c0p) echo "$(spec mbm-offs2-c0) -DAIOT_MOQT_SPLIT0_PREFETCH=ON";;
    # 10/13 hybrid supply: structure A with prefetch, plus the raw frame when the helper is free and no raster is ready,
    # admitted by the general rule (T_max 2 s, evidence slack, no activity gate)
    mbm-offs2-hy) echo "$(spec mbm-offs2-c0p) -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_GENERAL=ON -DAIOT_MOQT_GEN_QDELAY_MS=0 -DAIOT_MOQT_GEN_ACTIVE_MS=0 -DAIOT_MOQT_SPLIT0_HYBRID=ON";;
    # 10/13 mission first on core 0: the helper decoder at idle priority, paused while evidence is converted or waits
    mbm-offs2-c0m) echo "$(spec mbm-offs2-c0) -DAIOT_MOQT_SPLIT0_MISSION_FIRST=ON";;
    mbm-offs2-hym) echo "$(spec mbm-offs2-hy) -DAIOT_MOQT_SPLIT0_MISSION_FIRST=ON";;
    # 10/13 slack-based supply: the camera's own split stays, core 0 adds rasters only from the CPU the mission leaves
    mbm-offs2-ad) echo "$(spec mbm-offs2-c0m) -DAIOT_MOQT_SPLIT0_PREFETCH=ON -DAIOT_MOQT_SPLIT0_ADAPT=ON";;
    # 10/14 shared frame track: evidence = the detector's raster on offload/jpeg, fetched by the operator by Group;
    # the track keeps objects 10 s (evidence), a helper frame expires 1 s after publication
    mbm-offs2-sh) echo "$(spec mbm-offs2-m1) -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=10000 -DAIOT_MOQT_SHARED_FRAMES=ON -DAIOT_MOQT_SHARED_HELPER_MS=1000";;
    # 10/14 the shared frame track with the evidence property on its evidence frames (for the role-aware relay)
    mbm-offs2-sh2) echo "$(spec mbm-offs2-sh)";;
    # 10/15 night: the same with the sensor at 320x240 JPEG (full field of view; the detector decodes 1/2, 160x120): no UXGA decode on the camera
    mbm-offs2-sh2q) echo "$(spec mbm-offs2-sh) -DAIOT_CAMERA_QVGA=ON";;
    mbm-floor-a1q) echo "$(spec mbm-floor-a1) -DAIOT_CAMERA_QVGA=ON";;
    mbm-offs2-fq) echo "$(spec mbm-offs2-f) -DAIOT_CAMERA_QVGA=ON";;
    # 10/16: QVGA split with the camera's own inference at least every 1 s and the frames between to the helpers every 150 ms
    mbm-offs2-sh2qf) echo "$(spec mbm-offs2-sh2q) -DAIOT_MOQT_OFFLOAD_MS=150 -DAIOT_MOQT_SPLIT_LOCAL_MS=1000";;
    # 10/15 shared frame track with input offload: the helper gets the captured JPEG on offload/jpeg (it decodes; the camera decodes
    # nothing for it), the evidence is the detector raster with the evidence property; Chen rule with the helper stage of ~450 ms
    # 10/15 16:45 the same with the evidence before the next helper frame (MOQ_EV_WAITING; mbm-offc-sh starved the evidence)
    mbm-offc-se) echo "$(spec mbm-offc-sh)";;
    mbm-offc-sh) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=450 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=10000 -DAIOT_MOQT_OFFLOAD_SPLIT=OFF -DAIOT_MOQT_OFFLOAD_RASTER=85 -DAIOT_MOQT_SHARED_FRAMES=ON -DAIOT_MOQT_SHARED_INPUT=ON -DAIOT_MOQT_SHARED_HELPER_MS=1000";;
    mbm-offs2-f) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=310 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mqtt-offs2) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=310 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000 -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85 -DAIOT_MQTT_BASELINE=ON -DAIOT_MQTT_CA=$PWD/vendor/nbcert/cert.pem";;
    # 10/12 MQTT baseline of the split inference: the same application over MQTT 3.1.1/TLS/TCP (moq_mqtt_shim.c);
    # the frames to the helper on aiot/esp32/offload/jpeg (QoS 1), results QoS 1, evidence QoS 0, one connection
    mqtt-offs) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=400 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_CH_TMAX_MS=4000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=4000 -DAIOT_MOQT_OFFLOAD_SPLIT=ON -DAIOT_MOQT_OFFLOAD_RASTER=85 -DAIOT_MQTT_BASELINE=ON -DAIOT_MQTT_CA=$PWD/vendor/nbcert/cert.pem";;
    # 10/12: the same with the passage frame as the detector decoded it (1/8, 200x150), JPEG quality 85 (OFFLOAD_RASTER; the 1/8 re-decode of OFFLOAD_EIGHTH failed 3 in 4 and took 0.45 s, p8 pilot)
    mbm-offt8-all) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mbm-offt8-all-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mbm-offt8-ch) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mbm-offt8-ch-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=4 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mbm-offt8-ar) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=60000 -DAIOT_MOQT_TX_FIFO=ON -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mbm-offt8-ar-m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=500 -DAIOT_MOQT_OFFLOAD_TASK_MS=2000 -DAIOT_MOQT_CH_TMAX_MS=2000 -DAIOT_MOQT_OFFLOAD_DECOUPLE=ON -DAIOT_MOQT_IMAGE_SLOTS=2 -DAIOT_MOQT_OFFLOAD_MODE=3 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_RASTER=85";;
    mbm-off-v) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_MODE=2 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-vo20m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_MODE=2 -DAIOT_MOQT_VIGIL_CAPACITY_BPS=2500000 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-off-vo1m) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_MOQT_OFFLOAD_MS=1100 -DAIOT_MOQT_OFFLOAD_TIMEOUT_MS=2000 -DAIOT_MOQT_OFFLOAD_MODE=2 -DAIOT_MOQT_VIGIL_CAPACITY_BPS=125000 -DAIOT_MOQT_IMAGE_SLOTS=2";;
    mbm-floor-a0) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=0";;
    mbm-floor-a1) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1";;
    trace-mbm-floor) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_PASSAGE_ASSOC=1 -DAIOT_FRAME_TRACE_PORT=5310";;
    implicit-*) T=${1#implicit-}; echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_REPRESENTATION=1 -DAIOT_MOQT_TIMEOUT_FROM_CREATION=ON -DAIOT_MOQT_AI_DEADLINE_MS=$T -DAIOT_MOQT_MISSION_RESULT_TIMEOUT_MS=$T";;
    mbm-floor-t30) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_MUS_STATE_DELAY_MS=30";;
    mbm-floor-pred) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_MUS_PREDICT=ON";;
    mbm-pred) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_PREDICT=ON";;
    mbm-floor-wmm) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_WMM=ON";;
    prio-to-low-wmm) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_REPRESENTATION=1 -DAIOT_MOQT_WMM=ON";;
    mbm-noprobe) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=0";;
    *) return 1;;
  esac
}
methods=${*:-"fifo prio prio-to abr dtp bbr prio-to-tau prio-to-low prio-to-low-tau mqtt mbm mbm-noprobe"}
for m in $methods; do
  s=$(spec $m) || { echo "unknown method $m"; exit 1; }
  names="mx-a-$m$TAG mx-b-$m$TAG"; [ $m = mbm-noprobe ] && names="mx-a-$m$TAG"
  for n in $names; do
    flock vendor/build.lock tools/build_model.sh ${s%% *} $M $n $common ${s#* }
  done
done
