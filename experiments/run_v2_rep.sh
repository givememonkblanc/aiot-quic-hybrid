#!/bin/bash
# One v2 experiment rep: start receiver, reset device, capture, parse.
# Usage: run_v2_rep.sh <label> <rep> [capture_secs]
# Assumes the firmware for the desired APP_TRANSPORT_MODE is already flashed.
set -u
LABEL=${1:?usage: run_v2_rep.sh <label> <rep> [secs]}
REP=${2:?usage: run_v2_rep.sh <label> <rep> [secs]}
SECS=${3:-90}
ROOT=/home/ryzen395/aiot_quic_baseline
RUNDIR=$ROOT/experiments/runs_v2/${LABEL}_r${REP}
mkdir -p "$RUNDIR"

pkill -x picoquicdemo 2>/dev/null
sleep 1

AIOT_DGRAM_LOG="$RUNDIR/dgram_recv.csv" "$ROOT/host_receiver/run_server.sh" \
    > "$RUNDIR/server.log" 2>&1 &
SERVER_PID=$!
sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "receiver failed to start; see $RUNDIR/server.log" >&2
    exit 1
fi

python3 "$ROOT/experiments/capture_serial.py" --reset --seconds "$SECS" \
    --out "$RUNDIR/monitor.log"
CAPTURE_RC=$?

# run_server.sh is a bash wrapper: kill the wrapper AND the picoquicdemo
# child, which otherwise survives as an orphan holding port 14567
kill "$SERVER_PID" 2>/dev/null
pkill -x picoquicdemo 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
[ "$CAPTURE_RC" -ne 0 ] && exit "$CAPTURE_RC"

python3 "$ROOT/experiments/telemetry_analyzer/parse_aiot_monitor_log.py" \
    "$RUNDIR/monitor.log" --out-dir "$RUNDIR"
if [ -s "$RUNDIR/dgram_recv.csv" ]; then
    python3 "$ROOT/experiments/telemetry_analyzer/parse_receiver_dgram_log.py" \
        "$RUNDIR/dgram_recv.csv" --out-dir "$RUNDIR" > /dev/null
else
    echo "warning: no receiver datagram log for ${LABEL}_r${REP}"
fi
echo "rep complete: $RUNDIR"
