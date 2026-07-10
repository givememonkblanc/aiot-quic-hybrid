#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-/tmp/sf_test}"
gcc -O2 -Wall -Wextra -I "$ROOT/src/components/store_forward/include" \
    "$ROOT/experiments/store_forward_test.c" \
    "$ROOT/src/components/store_forward/store_forward.c" -o "$OUT"
"$OUT"
