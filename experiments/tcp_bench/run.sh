#!/bin/bash
# Start the TCP-MQTT baseline broker (TLS) + logging subscriber for one capture.
# Usage: run.sh <out_csv> [seconds]   (device must publish during the window)
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/tcp_dgram.csv}"; SECS="${2:-45}"
if [ ! -f "$DIR/certs/server.crt" ]; then
  mkdir -p "$DIR/certs"
  openssl req -x509 -newkey rsa:2048 -keyout "$DIR/certs/server.key" \
    -out "$DIR/certs/server.crt" -days 365 -nodes -subj "/CN=aiot-bench-broker" >/dev/null 2>&1
fi
cat > "$DIR/mosquitto.conf" <<CONF
listener 8883 0.0.0.0
allow_anonymous true
require_certificate false
certfile $DIR/certs/server.crt
keyfile $DIR/certs/server.key
CONF
pkill -x mosquitto 2>/dev/null || true; sleep 1
mosquitto -c "$DIR/mosquitto.conf" -d; sleep 1
echo "broker :8883 TLS up; logging $SECS s -> $OUT"
python3 "$DIR/subscriber.py" --host 127.0.0.1 --port 8883 --out "$OUT" --seconds "$SECS"
pkill -x mosquitto 2>/dev/null || true
