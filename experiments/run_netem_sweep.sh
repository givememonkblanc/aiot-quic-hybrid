#!/bin/bash
# netem RTT/loss sweep across transport modes (paper eval, Saif&Matrawy-style).
# Builds the EXPERIMENT firmware (APP_PRODUCT_MODE=0) per transport mode, then
# for each netem condition resets the device, captures a rep, and parses.
# Leaves netem OFF at the end. Restore product firmware separately afterwards.
#
#   experiments/run_netem_sweep.sh        # representative sweep
set -u
ROOT=/home/ryzen395/aiot_quic_baseline
CFG=$ROOT/src/main/app_config.h
IFACE=wlp194s0
SECS=${SECS:-45}
REPS=${REPS:-5}   # repetitions per condition (statistical: mean +/- CI)
export PATH="$PATH"
source /home/ryzen395/esp-idf/export.sh >/dev/null 2>&1

MODES=(stream_only datagram_only hybrid)
# label : netem verb : NETEM_ARGS   (bidir shapes uplink datagrams too)
CONDS=(
  "clean:off:"
  "rtt100:bidir:delay 50ms 10ms rate 8mbit"
  "rtt300:bidir:delay 150ms 20ms rate 8mbit"
  "rtt500:bidir:delay 250ms 30ms rate 8mbit"
  "loss2:bidir:delay 50ms 10ms loss 2% rate 8mbit"
  "loss10:bidir:delay 50ms 10ms loss 10% rate 8mbit"
)

mode_define() { case $1 in
  stream_only)   echo APP_TRANSPORT_MODE_STREAM_ONLY ;;
  datagram_only) echo APP_TRANSPORT_MODE_DATAGRAM_ONLY ;;
  hybrid)        echo APP_TRANSPORT_MODE_HYBRID ;;
esac ; }

# Credentials come from an untracked secrets file (never commit creds here).
# Create experiments/.sweep_secrets (chmod 600) with:
#   SWEEP_WIFI_SSID="..."  SWEEP_WIFI_PASSWORD="..."  SWEEP_PEER_HOST="192.168.0.38"
SECRETS="$ROOT/experiments/.sweep_secrets"
if [ ! -f "$SECRETS" ]; then
    echo "[sweep] missing $SECRETS (SWEEP_WIFI_SSID/PASSWORD/PEER_HOST). Aborting." >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$SECRETS"
: "${SWEEP_WIFI_SSID:?set in $SECRETS}" "${SWEEP_WIFI_PASSWORD:?set in $SECRETS}" "${SWEEP_PEER_HOST:?set in $SECRETS}"

echo "[sweep] backing up app_config.h"
cp "$CFG" "$CFG.sweepbak"
# Always restore the clean product config on exit (crash, Ctrl-C, or success),
# so experiment creds/mode never linger in the tree.
trap 'cp "$CFG.sweepbak" "$CFG" 2>/dev/null; rm -f "$CFG.sweepbak"' EXIT INT TERM

# bench experiment build: creds + fixed server (from secrets), product mode OFF
sed -i "s|#define APP_WIFI_SSID \"REPLACE_ME\"|#define APP_WIFI_SSID \"$SWEEP_WIFI_SSID\"|" "$CFG"
sed -i "s|#define APP_WIFI_PASSWORD \"REPLACE_ME\"|#define APP_WIFI_PASSWORD \"$SWEEP_WIFI_PASSWORD\"|" "$CFG"
sed -i "s|#define APP_PEER_HOST \"REPLACE_ME\"|#define APP_PEER_HOST \"$SWEEP_PEER_HOST\"|" "$CFG"
sed -i 's/#define APP_PRODUCT_MODE 1/#define APP_PRODUCT_MODE 0/' "$CFG"

for MODE in "${MODES[@]}"; do
  DEF=$(mode_define "$MODE")
  echo "==================== MODE=$MODE ($DEF) ===================="
  sed -i "s/#define APP_TRANSPORT_MODE APP_TRANSPORT_MODE_[A-Z_]*/#define APP_TRANSPORT_MODE $DEF/" "$CFG"
  echo "[sweep] building + flashing $MODE ..."
  if ! ( cd "$ROOT" && idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash ) \
        > "$ROOT/experiments/runs_v2/sweep_build_$MODE.log" 2>&1; then
    echo "[sweep] BUILD/FLASH FAILED for $MODE (see log)"; continue
  fi
  for C in "${CONDS[@]}"; do
    LBL=${C%%:*}; REST=${C#*:}; VERB=${REST%%:*}; ARGS=${REST#*:}
    echo "-------- $MODE / $LBL ($VERB $ARGS) --------"
    if [ "$VERB" = off ]; then
      sudo -n "$ROOT/experiments/netem_stress.sh" off "$IFACE" >/dev/null 2>&1
    else
      NETEM_ARGS="$ARGS" sudo -n NETEM_ARGS="$ARGS" "$ROOT/experiments/netem_stress.sh" "$VERB" "$IFACE" >/dev/null 2>&1
    fi
    for rep in $(seq 1 "$REPS"); do
      "$ROOT/experiments/run_v2_rep.sh" "sweep_${MODE}_${LBL}" "$rep" "$SECS" \
          >> "$ROOT/experiments/runs_v2/sweep_run.log" 2>&1 \
          && echo "  ok: sweep_${MODE}_${LBL}_r${rep}" || echo "  FAIL: sweep_${MODE}_${LBL}_r${rep}"
    done
    sudo -n "$ROOT/experiments/netem_stress.sh" off "$IFACE" >/dev/null 2>&1
  done
done

sudo -n "$ROOT/experiments/netem_stress.sh" off "$IFACE" >/dev/null 2>&1
echo "[sweep] DONE. Restore firmware: cp $CFG.sweepbak $CFG (product config kept in backup)."
