#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$TASK_ROOT"
export IDF_TOOLS_PATH="$TASK_ROOT/experiments/vlm_split/vendor/idf-tools"
source "$TASK_ROOT/experiments/vlm_split/vendor/esp-idf-v5.3.6/export.sh" >/dev/null 2>&1
for task_case in async_medium sync_medium async_low sync_low; do
 task_rep=3; task_async=OFF
 case "$task_case" in *_low) task_rep=1;; esac
 case "$task_case" in async_*) task_async=ON;; esac
 task_out="experiments/moq/results/codec-v22-firmware/$task_case"
 mkdir -p "$task_out"
 test ! -e "$task_out/firmware.bin"
 idf.py -B build_moq_screw_cap24 \
  -DAIOT_MOQT_ASYNC_CODEC="$task_async" -DAIOT_MOQT_CODEC_PIPELINE=OFF -DAIOT_MOQT_TRANSCODE=ON \
  -DAIOT_MOQT_REPLAY_INPUT=ON -DAIOT_EMBED_REPLAY_JPEG=ON \
  -DAIOT_REPLAY_JPEG="$TASK_ROOT/experiments/moq/fixtures/screw-ladder-v1/high.jpg" \
  -DAIOT_MOQT_LADDER_DIR= -DAIOT_MOQT_EVENT_EVIDENCE=ON \
  -DAIOT_MOQT_REPRESENTATION="$task_rep" -DAIOT_MOQT_FEASIBILITY=0 \
  -DAIOT_MOQT_DELIVERY_TIMEOUT_RESET=1 -DAIOT_MOQT_AI_DEADLINE_MS=10000 \
  -DAIOT_MOQT_IMAGE_SLOTS=1 -DAIOT_MOQT_PREVIEW_UNACKED_KB=32 \
  -DPROJECT_VER="moqt-codec-$task_case-v22" build > "$task_out/build.log" 2>&1
 cp build_moq_screw_cap24/aiot_quic.bin "$task_out/firmware.bin"
 cp build_moq_screw_cap24/aiot_quic.elf "$task_out/firmware.elf"
 python experiments/moq/freeze_codec_build.py "$task_out"
 printf 'Built %s\n' "$task_case"
done
