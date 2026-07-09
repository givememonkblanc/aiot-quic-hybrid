#!/bin/bash
# Scenario testbed matrix: 3 transport modes x N reps x 95s under
# BIDIRECTIONAL netem (uplink datagrams see loss/delay too).
# Each rep runs the 3-phase timeline (normal -> camera-image burst ->
# recovery) via APP_SCENARIO_MODE_ENABLE. netem is removed on exit.
# Usage: run_v2_scenario_matrix.sh [reps]
set -u
REPS=${1:-5}
ROOT=/home/ryzen395/aiot_quic_baseline
CFG=$ROOT/src/main/app_config.h
source /home/ryzen395/esp-idf/export.sh >/dev/null 2>&1

set_mode() {
    sed -i "s/#define APP_TRANSPORT_MODE APP_TRANSPORT_MODE_[A-Z_]*/#define APP_TRANSPORT_MODE $1/" "$CFG"
}
set_scenario() {
    sed -i "s/#define APP_SCENARIO_MODE_ENABLE [01]/#define APP_SCENARIO_MODE_ENABLE $1/" "$CFG"
}

cleanup() {
    "$ROOT/experiments/netem_stress.sh" off >/dev/null 2>&1
    set_scenario 0
    set_mode APP_TRANSPORT_MODE_HYBRID
}
trap cleanup EXIT

set_scenario 1
"$ROOT/experiments/netem_stress.sh" bidir || { echo "netem setup failed"; exit 1; }

for MODE in hybrid stream_only datagram_only; do
    case $MODE in
        hybrid)        set_mode APP_TRANSPORT_MODE_HYBRID ;;
        stream_only)   set_mode APP_TRANSPORT_MODE_STREAM_ONLY ;;
        datagram_only) set_mode APP_TRANSPORT_MODE_DATAGRAM_ONLY ;;
    esac
    echo "=== building+flashing ${MODE}_scenario $(date +%T) ==="
    if ! (cd "$ROOT" && idf.py build && idf.py -p /dev/ttyACM0 flash) > "$ROOT/experiments/runs_v2/build_${MODE}_scenario.log" 2>&1; then
        echo "BUILD/FLASH FAILED for $MODE — aborting"
        exit 1
    fi
    for REP in $(seq 1 "$REPS"); do
        echo "=== ${MODE}_scenario rep $REP $(date +%T) ==="
        "$ROOT/experiments/run_v2_rep.sh" "${MODE}_scenario" "$REP" 95 || echo "REP FAILED: ${MODE}_scenario r$REP"
        sleep 3
    done
done
echo "SCENARIO MATRIX DONE $(date +%T)"
