#!/bin/bash
# Full v2 matrix: 3 transport modes x 5 reps x 90s.
# Edits APP_TRANSPORT_MODE in app_config.h per mode, rebuilds, flashes, runs reps.
# Usage: run_v2_matrix.sh [label_suffix]   e.g. "_stress" (netem managed by caller)
set -u
SUFFIX=${1:-}
ROOT=/home/ryzen395/aiot_quic_baseline
CFG=$ROOT/src/main/app_config.h
source /home/ryzen395/esp-idf/export.sh >/dev/null 2>&1

set_mode() {
    sed -i "s/#define APP_TRANSPORT_MODE APP_TRANSPORT_MODE_[A-Z_]*/#define APP_TRANSPORT_MODE $1/" "$CFG"
}

for MODE in hybrid stream_only datagram_only; do
    case $MODE in
        hybrid)        set_mode APP_TRANSPORT_MODE_HYBRID ;;
        stream_only)   set_mode APP_TRANSPORT_MODE_STREAM_ONLY ;;
        datagram_only) set_mode APP_TRANSPORT_MODE_DATAGRAM_ONLY ;;
    esac
    echo "=== building+flashing $MODE $(date +%T) ==="
    if ! (cd "$ROOT" && idf.py build && idf.py -p /dev/ttyACM0 flash) > "$ROOT/experiments/runs_v2/build_$MODE.log" 2>&1; then
        echo "BUILD/FLASH FAILED for $MODE — aborting"
        exit 1
    fi
    for REP in 1 2 3 4 5; do
        echo "=== ${MODE}${SUFFIX} rep $REP $(date +%T) ==="
        "$ROOT/experiments/run_v2_rep.sh" "${MODE}${SUFFIX}" "$REP" 90 || echo "REP FAILED: ${MODE}${SUFFIX} r$REP"
        sleep 3
    done
done

set_mode APP_TRANSPORT_MODE_HYBRID
echo "MATRIX DONE $(date +%T)"
