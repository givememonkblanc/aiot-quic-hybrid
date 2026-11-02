#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$TASK_ROOT"
# Requires the isolated ESPDet SDK/model setup and temporary 8 MiB config.
python3 - <<'PY'
from pathlib import Path
p = Path('experiments/vlm_split/integrated/sdkconfig')
s = p.read_text()
import re
s, n = re.subn(r'^CONFIG_ESP_MAIN_TASK_STACK_SIZE=\d+$', 'CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288', s, flags=re.M)
assert n == 1
p.write_text(s)
PY
bash experiments/vlm_split/build_integrated.sh \
 -D AIOT_ESP32_HW_AES=OFF -D AIOT_QUIC_PSRAM_POOL=ON \
 -D AIOT_QUIC_CIPHER_LIST=TLS13-CHACHA20-POLY1305-SHA256
