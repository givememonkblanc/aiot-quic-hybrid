#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export IDF_TOOLS_PATH="$TASK_ROOT/vendor/idf-tools"
source "$TASK_ROOT/vendor/esp-idf-v5.3.6/export.sh"
cd "$TASK_ROOT/espdet_probe"
idf.py -B build -D PROJECT_VER=espdet-probe-v1 build
