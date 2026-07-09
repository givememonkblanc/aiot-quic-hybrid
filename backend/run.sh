#!/bin/bash
# Launch the AIoT product backend: the MQTT-over-QUIC receiver (picoquic) that
# writes the unified feed, plus the dashboard that ingests it. Ctrl-C stops both.
#
#   ./backend/run.sh [DASHBOARD_PORT]
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-8080}"
FEED="${AIOT_FEED_LOG:-$ROOT/backend/aiot_feed.jsonl}"
IMG="${AIOT_IMAGE:-$ROOT/backend/latest.jpg}"   # live-view frame (receiver writes, apps read)
RECEIVER="$ROOT/host_receiver"

command -v python3 >/dev/null || { echo "python3 required"; exit 1; }
[ -x "$RECEIVER/picoquic/picoquicdemo" ] || { echo "build picoquicdemo first (host_receiver/picoquic)"; exit 1; }

echo "[run] feed=$FEED  dashboard=http://localhost:$PORT/"
pkill -x picoquicdemo 2>/dev/null || true
sleep 0.5

# MQTT-over-QUIC receiver -> unified feed (+ legacy CSVs) + live-view frame
( cd "$RECEIVER" && AIOT_FEED_LOG="$FEED" AIOT_IMAGE_OUT="$IMG" \
    ./picoquic/picoquicdemo -p 14567 \
      -c certs/cert.pem -k certs/key.pem -a mqtt -q qlog_dir ) &
SRV=$!
sleep 1

# Spaces service (space-utilization application) on PORT+10, background.
SPACE_PORT=$((PORT + 10))
DB="$ROOT/backend/aiot.db"
AIOT_FEED_LOG="$FEED" AIOT_DB="$DB" \
    python3 "$ROOT/backend/spaceapp.py" --port "$SPACE_PORT" --feed "$FEED" --db "$DB" &
SPACE=$!
trap 'kill $SRV $SPACE 2>/dev/null || true; pkill -x picoquicdemo 2>/dev/null || true' EXIT INT TERM
echo "[run] spaces  = http://localhost:$SPACE_PORT/"

# Management app + dashboard (foreground)
AIOT_FEED_LOG="$FEED" AIOT_DB="$DB" AIOT_IMAGE="$IMG" \
    python3 "$ROOT/backend/dashboard.py" --port "$PORT" --feed "$FEED" --db "$DB" --image "$IMG"
