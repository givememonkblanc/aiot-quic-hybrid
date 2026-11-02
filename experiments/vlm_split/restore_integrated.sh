#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$TASK_ROOT"
python3 - <<'PY'
import hashlib,json
from pathlib import Path
b=Path('experiments/ota/private/integrated-20260921/flash-before.bin').read_bytes()
m=json.loads(Path('experiments/vlm_split/results/flash-backup-manifest.json').read_text())
assert len(b)==0x800000 and hashlib.sha256(b).hexdigest()==m['sha256']
print('Full-flash restore backup verified')
PY
/home/ryzen395/.espressif/python_env/idf5.2_py3.12_env/bin/python -m esptool \
 --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash \
 --flash_mode keep --flash_freq keep --flash_size keep \
 0 experiments/ota/private/integrated-20260921/flash-before.bin
