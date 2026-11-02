#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$TASK_ROOT"
# Serial ownership is strictly sequential: esptool -> measurement -> restore.
trap 'experiments/vlm_split/restore_integrated.sh > experiments/quic_batch/results/restore-flash.log 2>&1' EXIT
/home/ryzen395/.espressif/python_env/idf5.2_py3.12_env/bin/python -m esptool \
 --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash \
 --flash_mode dio --flash_size 8MB --flash_freq 80m \
 0x0 experiments/vlm_split/integrated/build/bootloader/bootloader.bin \
 0x8000 experiments/vlm_split/integrated/build/partition_table/partition-table.bin \
 0x10000 experiments/vlm_split/integrated/build/aiot_quic.bin \
 0x3f0000 experiments/vlm_split/integrated/build/ota_data_initial.bin \
 > experiments/quic_batch/results/formal-flash.log 2>&1
python experiments/quic_batch/trial.py > experiments/quic_batch/results/formal-console.log 2>&1
