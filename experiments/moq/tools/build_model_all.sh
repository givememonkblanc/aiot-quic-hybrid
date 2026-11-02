#!/usr/bin/env bash
# Builds every method of the alert campaign with a trained screw model, one image at a time,
# into am-<method>; existing images are kept. The inference task runs on core 1 at priority 1:
# on core 0 at the priority of the idle task it got half the core next to the network, and the
# result interval grew from 1.25 to 1.7 s. usage: [IMAGES=am] build_model_all.sh <model dir> <method>...
# IMAGES=an: the results are binary (AIOT_MOQT_RESULT_BINARY) for every method, and the
# scheduler with a state delay cuts its evidence window to it (AIOT_MOQT_MUS_FINE_WINDOW).
# IMAGES=ao: as an, and the scheduler with a state delay probes the connection while a result
# waits for its acknowledgement (AIOT_MOQT_RESULT_PROBE_MS=50).
# IMAGES=aq: the images of ao under another name, so another version string and another image
# hash: the relay runs alternate ao and aq so that every run installs an image and restarts.
exec 9> /home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/build.lock
flock -n 9 || { echo "another build is running"; exit 1; }
V=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor
M=$1; shift
I=${IMAGES:-am}
for m in "$@"; do
  [ -f $V/build-draft21-s3-$I-$m/aiot_quic.bin ] && { echo "$I-$m exists"; continue; }
  extra=""
  if [ "$I" = an ] || [ "$I" = ao ] || [ "$I" = aq ]; then
    extra="-DAIOT_MOQT_RESULT_BINARY=ON"
    case $m in mus-alert|mus-1000) extra="$extra -DAIOT_MOQT_MUS_FINE_WINDOW=ON";; esac
  fi
  { [ "$I" = ao ] || [ "$I" = aq ]; } && case $m in mus-alert|mus-1000) extra="$extra -DAIOT_MOQT_RESULT_PROBE_MS=50";; esac
  /home/ryzen395/aiot_quic_baseline/experiments/moq/tools/build_model.sh $m $M $I-$m -DAIOT_PROFILE_AI_CORE=1 -DAIOT_PROFILE_AI_PRIORITY=1 $extra
done
echo BUILD_${I^^}_DONE
