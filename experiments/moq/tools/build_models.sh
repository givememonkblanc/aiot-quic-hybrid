#!/usr/bin/env bash
# Images of the model-size pilot: mbm-floor and prio-to-low (tools/build_matrix.sh) with a larger
# screw detector, under the names mx-{a,b}-<method>-<model>. f1 is the model of the matrix.
#   e1   80x60 input, 59 kB, arena 256 KiB (decoding at 1/8)
#   v4   160x120 input, 86 kB, arena 640 KiB (decoding at 1/4: the region of 1600x1200 at 1/4 is 160x120)
#   vid-*   video workload: every camera frame at full resolution on the media track (latest frame,
#           D = 5 s) with the results; prio (priority + timeout), fix16k (16 KiB of unacknowledged
#           media), mbm (window = rate x 100 ms)
#   x2, x4  f1 run 2 and 4 times on every frame (AIOT_AI_REPEAT): the time of a slower model with
#           the accuracy of f1
# usage: [METHODS="mbm-floor prio-to-low mbm-floor-pred"] build_models.sh [model...]   (default: v4 e1)
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
D=/home/ryzen395/data/aiot_quic_results/screw_turntable
common="-DAIOT_PROFILE_AI_CORE=1 -DAIOT_PROFILE_AI_PRIORITY=1 -DAIOT_MOQT_RESULT_BINARY=ON -DAIOT_MOQT_RUN_SECONDS=213"
spec() {
  case $1 in
    mbm-floor) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000";;
    prio-to-low) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_REPRESENTATION=1";;
    vid-prio) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_EVENT_EVIDENCE=OFF -DAIOT_MOQT_PREVIEW_DEADLINE_MS=5000 -DAIOT_MOQT_REPRESENTATION=0 -DAIOT_MOQT_TRACE_MS=100";;
    vid-fix16k) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=0 -DAIOT_MOQT_EVENT_EVIDENCE=OFF -DAIOT_MOQT_PREVIEW_DEADLINE_MS=5000 -DAIOT_MOQT_REPRESENTATION=0 -DAIOT_MOQT_TRACE_MS=100 -DAIOT_MOQT_PREVIEW_UNACKED_KB=16";;
    vid-mbm) echo "timeout-med -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_EVENT_EVIDENCE=OFF -DAIOT_MOQT_PREVIEW_DEADLINE_MS=5000 -DAIOT_MOQT_REPRESENTATION=0 -DAIOT_MOQT_TRACE_MS=100 -DAIOT_MOQT_VIDEO_WINDOW_MS=100";;
    mbm-floor-fix16k) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_MUS_FIXED_WINDOW=16384";;
    mbm-floor-pred) echo "mus-alert -DAIOT_MOQT_RESULT_PROBE_MS=50 -DAIOT_MOQT_MUS_QUALITY_LOW=1000 -DAIOT_MOQT_MUS_QUALITY_MEDIUM=1000 -DAIOT_MOQT_MUS_PREDICT=ON";;
  esac
}
scale() { case $1 in v4) echo "-DAIOT_AI_ROI_SCALE=4";; x2) echo "-DAIOT_AI_REPEAT=2";; x4) echo "-DAIOT_AI_REPEAT=4";; *) echo "";; esac; }
dir() { case $1 in x2|x4) echo $D/model-f1;; *) echo $D/model-$1;; esac; }
for model in ${*:-v4 e1}; do
  for m in ${METHODS:-mbm-floor prio-to-low}; do
    s=$(spec $m)
    for n in mx-a-$m-$model mx-b-$m-$model; do
      flock vendor/build.lock tools/build_model.sh ${s%% *} $(dir $model) $n $common ${s#* } $(scale $model)
    done
  done
done
