#!/bin/bash
# Build and run the semantic-transport decision matrix (host, no device/netem).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-/tmp/semantic_matrix}"
gcc -O2 -Wall -I "$ROOT/src/components/dl_qads/include" \
    "$ROOT/experiments/semantic_decision_matrix.c" \
    "$ROOT/src/components/dl_qads/dl_qads.c" -o "$OUT"
"$OUT"
