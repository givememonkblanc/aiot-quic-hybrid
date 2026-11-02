#!/usr/bin/env bash
# Builds every method of the turntable campaign, one image at a time. A second copy of this
# script exits at once, and images that exist are not built again.
exec 9> /home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/build.lock
flock -n 9 || { echo "another build is running"; exit 1; }
V=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor
for m in mus-full prio-high prio-med timeout-high timeout-med abr feasible mus-norisk mus-noahead mus; do
  [ -f $V/build-draft21-s3-rt-$m-a/aiot_quic.bin ] && [ -f $V/build-draft21-s3-rt-$m-b/aiot_quic.bin ] && { echo "rt-$m exists"; continue; }
  /home/ryzen395/aiot_quic_baseline/experiments/moq/tools/build_turntable.sh $m
done
echo BUILD_RT_DONE
