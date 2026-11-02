#!/usr/bin/env bash
# Baseline campaign on the real turntable: the relay of this work (policy alert) against two other
# servers over the same first hop (512 kbit/s, 5 ms) and the same four subscriber links and outages
# per round as campaign_relay.sh.
#   alert   relay_receiver.py, the MOQT relay of this work; the device runs the proposed method
#   imquic  baseline_receiver.py --server imquic, the draft-21 relay of Meetecho imquic; same device
#   mqtt    baseline_receiver.py --server mqtt, a mosquitto broker; the device runs the MQTT baseline
#           (moq_mqtt_shim.c: results QoS 1, images QoS 0 at the medium representation, persistent
#           session), and the first hop shapes TCP instead of UDP
# usage: [PREFIX=bl1] [LONG="mon:30:10 qa:70:15"] [LATEST=HHMM] campaign_baseline2.sh <rounds> <server>...
# Every run installs an image and restarts the device: two builds of each image under two names are
# alternated, and neither the running image nor the last one downloaded is chosen (the device refuses
# the image of its last attempt). A failed run leaves its image unconfirmed and the device returns to
# the previous image by itself; the campaign waits for that and goes on, and pauses after two failures
# in a row or when the device does not return. Pause between runs with the file `pause`.
S=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/alert-state; mkdir -p $S
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
P=vendor/venv/bin/python
PREFIX=${PREFIX:-bl1}
SUBS="--receiver-arg=--sub --receiver-arg=ctrl:f128:results --receiver-arg=--sub --receiver-arg=mon:f128:all --receiver-arg=--sub --receiver-arg=qa:f512:all --receiver-arg=--sub --receiver-arg=arch:trace:all"
rounds=$1; shift
elf() { python3 -c "import sys;print(open(sys.argv[1],'rb').read()[176:208].hex())" $1; }
current() { grep -o "current=[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d= -f2; }
last() { grep -o "/images/[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d/ -f3; }
# after the given line of the service log, the device polls with a confirmed image after its last download
recovered() { tail -n +$1 ../ota/results/service.log | awk '/\/images\//{ok=0} /\/next.*pending=0/{ok=1} END{exit !ok}'; }
fails=0
for attempt in 1 2; do for r in $(seq 1 $rounds); do for srv in "$@"; do
  n=$PREFIX-$srv-r$r
  grep -q '"status": "complete"' results/$n/manifest.json 2>/dev/null && [ -s results/$n/serial.log ] && continue
  while [ -f $S/pause ]; do touch $S/paused; read -t 5 <> <(:); done; rm -f $S/paused
  [ -n "$LATEST" ] && [ "$(date +%H%M)" -ge "$LATEST" ] && { echo "$(date +%T) stopped: after $LATEST"; break 3; }
  case $srv in
    mqtt) a=mq-prio-med; b=mr-prio-med; recv=baseline_receiver.py; extra="--receiver-arg=--server --receiver-arg=mqtt --ip-proto tcp";;
    imquic) a=ao-mus-alert; b=aq-mus-alert; recv=baseline_receiver.py; extra="--receiver-arg=--server --receiver-arg=imquic";;
    alert) a=ao-mus-alert; b=aq-mus-alert; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=alert";;
    *) echo "unknown server $srv"; exit 1;;
  esac
  F=$PWD/vendor/build-draft21-s3-$a/aiot_quic.bin
  case "$(elf $F)" in "$(current)"|"$(last)") F=$PWD/vendor/build-draft21-s3-$b/aiot_quic.bin;; esac
  rm -rf results/$n
  from=$(( $(wc -l < ../ota/results/service.log) + 1 ))
  long=""; for l in $LONG; do long="$long --receiver-arg=--long --receiver-arg=$l"; done
  $P $PWD/remote_ota_trial.py --name $n --firmware $F --receiver $recv $extra $SUBS $long \
    --receiver-arg=--seed --receiver-arg=$r --receiver-arg=--trace-offset --receiver-arg=$(( (r-1)*130 )) \
    --shaped --delay-ms 5 --rate-kbit 512 --timeout 330 --no-restore --netlog > $S/trial-$n.log 2>&1
  st=$(grep -o '"status": "[a-z]*"' results/$n/manifest.json 2>/dev/null)
  ev=$(grep -c MOQ_PASSAGE results/$n/serial.log 2>/dev/null)
  echo "$(date +%T) $n $st passages $ev"
  if [ "${ev:-0}" -lt 8 ]; then
    mv results/$n results/rejected-$n-$(date +%s); fails=$((fails+1))
    t=0
    until recovered $from || [ $t -ge 600 ]; do read -t 10 <> <(:); t=$((t+10)); done
    if [ $fails -ge 2 ] || ! recovered $from; then
      touch $S/pause; echo "$(date +%T) PAUSED: only ${ev:-0} passages in $n (failures $fails)"
    else
      echo "$(date +%T) rejected $n (${ev:-0} passages); the device returned to $(current | cut -c1-12)"
    fi
    continue
  fi
  fails=0
  burst=$(python3 tools/longest_burst.py results/$n/serial.log 2>/dev/null)
  if [ "${burst:-0}" -gt 25 ]; then
    mv results/$n results/rejected-$n-$(date +%s); touch $S/pause; echo "$(date +%T) PAUSED: a burst of $burst passages in $n (light?)"
  fi
done; done; done
echo CAMPAIGN_DONE
