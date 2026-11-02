#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
export AIOT_ESPDET_RUNTIME=1
export IDF_TOOLS_PATH="$TASK_ROOT/experiments/vlm_split/vendor/idf-tools"
source "$TASK_ROOT/experiments/vlm_split/vendor/esp-idf-v5.3.6/export.sh"
cd "$TASK_ROOT"
idf.py -B experiments/vlm_split/integrated/build \
 -D SDKCONFIG="$TASK_ROOT/experiments/vlm_split/integrated/sdkconfig" \
 -D AIOT_MOBILITY_RESEARCH=ON -D AIOT_MOBILITY_CAMERA=ON -D AIOT_ESPDET_RUNTIME=ON \
 -D AIOT_PIPELINE_PROFILE=OFF -D AIOT_QUIC_BATCH_EXPERIMENT=OFF \
 -D AIOT_PROFILE_AI_CORE=1 -D PROJECT_VER=espdet-integrated-v1 \
 -D AIOT_OTA_SITE_HEADER="$TASK_ROOT/experiments/ota/private/ota_site.h" \
 -D AIOT_REPLAY_JPEG="$TASK_ROOT/experiments/vlm_split/vendor/esp-dl/examples/cat_detect/main/cat.jpg" "$@" build
