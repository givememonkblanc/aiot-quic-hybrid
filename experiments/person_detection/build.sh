#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
export AIOT_CAMERA_MODEL_SOURCE="$ROOT/experiments/person_detection/out/ai_inference_vehicle_model.c"
export AIOT_CAMERA_MODEL_METADATA="$ROOT/experiments/person_detection/out/person_asset.h"
bash "$ROOT/experiments/ota/build.sh" "${1:?Supply a unique app version}"
