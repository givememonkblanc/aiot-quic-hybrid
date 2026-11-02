#!/usr/bin/env bash
# Helper ESP32 (AIOT_MOQT_HELPER, src/main/app_moq_helper.inc): build, flash, bench.
#
#   helper_bench.sh build [NAME] [-D...]   firmware into vendor/build-draft21-s3-NAME
#                                          (default NAME=helper: relay 192.168.0.38:14600,
#                                          reports to 192.168.0.38:5320)
#   helper_bench.sh build-bench            NAME=helper-bench: relay port 15600, reports to 15320
#   helper_bench.sh flash [NAME] [PORT]    USB flash (default /dev/ttyACM0); close any serial
#                                          monitor first, esptool fails while one is open
#   helper_bench.sh bench OUT [FRAMES] [helper_bench.py run options, e.g. --camera-frames --period 0.4]
#                                          relay on 15468/15600 + model camera (trace frames,
#                                          1 per s) + serial log + subscriber of helper/events
#                                          (helper_events_sub.py), then the analysis
#
# The build follows tools/build_model.sh: base timeout-med (s2), the screw model model-f1, ROI x8,
# no dark-blob detector, inference on core 1 at priority 1 like the camera, no publisher state kept
# across sessions (KEEP_CACHE off: a new session after a camera restart publishes afresh). Netlog is off: the
# helper must not write into the camera's console copy (UDP 14570).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
MODEL=${MODEL:-/home/ryzen395/data/aiot_quic_results/screw_turntable/model-f1}
PY_ESPTOOL=/home/ryzen395/.espressif/python_env/idf5.2_py3.12_env/bin/python
HELPER_OPTS=(-DAIOT_NETLOG_HOST= -DAIOT_PROFILE_AI_CORE=1 -DAIOT_PROFILE_AI_PRIORITY=1 -DAIOT_MOQT_HELPER=ON -DAIOT_MOQT_KEEP_CACHE=OFF -DAIOT_MOQT_RECONNECT_LIMIT=0)

build() {
  local name=$1; shift
  flock vendor/build.lock nice tools/build_model.sh timeout-med "$MODEL" "$name" "${HELPER_OPTS[@]}" "$@"
}

case ${1:-} in
  build) shift; name=${1:-helper}; shift || true; build "$name" "$@";;
  build-bench) build helper-bench -DAIOT_MOQT_HELPER_PORT=15600 -DAIOT_MOQT_HELPER_REPORT_PORT=15320;;
  flash)
    name=${2:-helper}; port=${3:-/dev/ttyACM0}; B=vendor/build-draft21-s3-$name
    # bootloader, partition table (OTA layout), the image in ota_0 and an initial otadata, so the
    # board boots ota_0; the helper confirms the slot once it is subscribed
    (cd "$B" && $PY_ESPTOOL -m esptool --chip esp32s3 -p "$port" -b 921600 --before default_reset --after hard_reset \
       write_flash $(tr '\n' ' ' < flash_args));;
  bench)
    out=$(realpath -m "${2:?OUT}"); frames=${3:-120}; shift 3 2>/dev/null || shift $#; mkdir -p "$out"
    relay/moq_relay --up 0.0.0.0:15468 --down 0.0.0.0:15600 --cert private/cert.pem --key private/key.pem \
      --offload --helper-events --log "$out/relay.jsonl" 2> "$out/relay.err" & relay=$!
    # the results of the helper, over MoQ through the relay (track helper/events)
    vendor/venv/bin/python helper_events_sub.py --connect 127.0.0.1:15600 --seconds 100000 --out "$out/events.jsonl" > "$out/events.out" 2>&1 & sub=$!
    python3 serial_log.py /dev/ttyACM0 "$out/serial.log" & serial=$!
    trap 'kill $relay $serial $sub 2>/dev/null' EXIT
    sleep 1
    # the helper reconnects every 2 s, so it finds the new relay by itself
    vendor/venv/bin/python helper_bench.py run --out "$out" --frames "$frames" --wait 8 --drain 6 "$@"
    kill $relay $serial $sub 2>/dev/null; wait 2>/dev/null; trap - EXIT
    python3 helper_bench.py analyze "$out";;
  *) sed -n 2,14p "$0"; exit 2;;
esac
