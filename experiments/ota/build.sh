#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${1:?Usage: bash experiments/ota/build.sh VERSION}"
cd "$ROOT"
if [[ ! -f experiments/ota/private/ota_site.h ]]; then
  echo 'Initialize the HTTPS site with service.py init first.' >&2
  exit 1
fi
python3 experiments/ota/prepare_config.py
idf.py -B build_mobility_ota -D SDKCONFIG=sdkconfig.mobility_ota \
  -D AIOT_MOBILITY_RESEARCH=ON -D AIOT_MOBILITY_CAMERA=ON \
  -D AIOT_PROFILE_AI_CORE="${AIOT_PROFILE_AI_CORE:-0}" \
  -D AIOT_OTA_SITE_HEADER="$ROOT/experiments/ota/private/ota_site.h" \
  -D AIOT_CAMERA_MODEL_SOURCE="${AIOT_CAMERA_MODEL_SOURCE:-$ROOT/src/components/ai_inference/ai_inference_vehicle_model.c}" \
  -D AIOT_CAMERA_MODEL_METADATA="${AIOT_CAMERA_MODEL_METADATA:-$ROOT/src/components/ai_inference/include/ai_inference_model_asset_generated.h}" \
  -D PROJECT_VER="$VERSION" build
