#!/usr/bin/env bash
# Builds every method of the alert campaign, one image at a time; existing images are kept.
exec 9> /home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/build.lock
flock -n 9 || { echo "another build is running"; exit 1; }
V=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor
for m in "$@"; do
  [ -f $V/build-draft21-s3-al-$m/aiot_quic.bin ] && { echo "al-$m exists"; continue; }
  /home/ryzen395/aiot_quic_baseline/experiments/moq/tools/build_alert.sh $m
done
echo BUILD_AL_DONE
