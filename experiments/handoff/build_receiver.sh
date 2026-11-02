#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PTLS="$ROOT/host_receiver/picotls"
cmake -S "$ROOT/host_receiver/picoquic" -B "$ROOT/build_handoff_receiver" \
  -DPICOQUIC_FETCH_PTLS=OFF -DPTLS_INCLUDE_DIR="$PTLS/include" \
  -DPTLS_CORE_LIBRARY="$PTLS/libpicotls-core.a" \
  -DPTLS_OPENSSL_LIBRARY="$PTLS/libpicotls-openssl.a" \
  -DPTLS_FUSION_LIBRARY="$PTLS/libpicotls-fusion.a" \
  -DPTLS_MINICRYPTO_LIBRARY="$PTLS/libpicotls-minicrypto.a"
cmake --build "$ROOT/build_handoff_receiver" -j "${JOBS:-8}"
"$ROOT/build_handoff_receiver/picoquic_ct" -S "$ROOT/host_receiver/picoquic" \
  frames_parse migration migration_with_loss migration_controlled
