#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
python3 "$TASK_ROOT/experiments/quic_batch/prepare_runtime_counter.py"
exec "$TASK_ROOT/experiments/vlm_split/build_integrated.sh" \
 -D AIOT_QUIC_BATCH_EXPERIMENT=ON \
 -D AIOT_ESP32_HW_AES=OFF -D AIOT_QUIC_PSRAM_POOL=ON \
 -D AIOT_QUIC_CIPHER_LIST=TLS13-CHACHA20-POLY1305-SHA256 \
 -D AIOT_REPLAY_JPEG="$TASK_ROOT/experiments/quic_batch/private/trace.jpg"
