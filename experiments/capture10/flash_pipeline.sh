#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$TASK_ROOT"
# Only start after the live-FOMO serial session is fully closed.
python3 - <<'PY'
from pathlib import Path
import hashlib
p=Path('experiments/ota/private/integrated-20260921/flash-before.bin')
assert p.stat().st_size==8388608 and hashlib.sha256(p.read_bytes()).hexdigest()=='6ddb628302bb58777aded5267f3d4efd6057840e8d24551f0727e215a3d5a93e'
PY
trap 'experiments/vlm_split/restore_integrated.sh > experiments/capture10/results/restore-flash.log 2>&1' EXIT
/home/ryzen395/.espressif/python_env/idf5.2_py3.12_env/bin/python -m esptool \
 --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash \
 --flash_mode dio --flash_size 8MB --flash_freq 80m \
 0x0 experiments/vlm_split/integrated/build/bootloader/bootloader.bin \
 0x8000 experiments/vlm_split/integrated/build/partition_table/partition-table.bin \
 0x10000 experiments/vlm_split/integrated/build/aiot_quic.bin \
 0x3f0000 experiments/vlm_split/integrated/build/ota_data_initial.bin \
 > experiments/capture10/results/flash.log 2>&1
python experiments/capture10/pipeline_trial.py > experiments/capture10/results/pipeline-console.log 2>&1
