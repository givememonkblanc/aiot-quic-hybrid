#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$TASK_ROOT"
export IDF_TOOLS_PATH="$TASK_ROOT/experiments/vlm_split/vendor/idf-tools"
source "$TASK_ROOT/experiments/vlm_split/vendor/esp-idf-v5.3.6/export.sh" >/dev/null 2>&1
# Existing OTA-size config fixes screw FOMO, AI core, cipher, memory and slots.
# The same ladder is offered to the build; the linker may discard unused levels.
# Record binary sizes; do not claim identical flash layout across variants.
for task_case in high high_timeout low medium; do
  task_rep=0; task_timeout=1
  case "$task_case" in high) task_timeout=0;; low) task_rep=1;; medium) task_rep=3;; esac
  task_out="experiments/moq/results/ladder-v17-firmware/$task_case"
  mkdir "$task_out"
  idf.py -B build_moq_screw_cap24 \
    -D AIOT_MOQT_REPRESENTATION="$task_rep" -D AIOT_MOQT_FEASIBILITY=0 \
    -D AIOT_MOQT_DELIVERY_TIMEOUT_RESET="$task_timeout" \
    -D PROJECT_VER="moqt-ladder-$task_case-v17" build > "$task_out/build.log" 2>&1
  cp build_moq_screw_cap24/aiot_quic.bin "$task_out/firmware.bin"
  cp build_moq_screw_cap24/aiot_quic.elf "$task_out/firmware.elf"
  printf 'Built %s\n' "$task_case"
done
