#!/usr/bin/env bash
# Relay campaign (RELAY_EXPERIMENT.md) on the real turntable: the device publishes to the MOQT
# relay on the host over a first hop shaped to 512 kbit/s with 5 ms delay and no loss, so that
# it chooses the representations of a 512 kbit/s link and the policies differ on the second hop
# only. Four subscribers receive through the relay, each behind its own emulated link: the
# controller of the line (results only, 128 kbit/s), a monitor (results and images, 128 kbit/s),
# a quality station (results and images, 512 kbit/s), and an archive (results and images, the
# measured Wi-Fi trace at 256 kbit/s). The links of the subscribers carry the burst loss, jitter
# and outages of the alert campaign, drawn per round, the same for every policy of a round.
# usage: [PREFIX=rl1] [LONG="qa:40:10"] [RELAY_ARGS="--no-cache"] campaign_relay.sh <rounds> <policy>...
# FIRST=impaired: the first hop carries the burst loss, jitter and short outages of the alert
# campaign as well (outages drawn from seed 10*round+9), so that the impairments of the Wi-Fi of
# the device meet the relay. Default: clean first hop.
# LONG: outages of a subscriber link that outlast the idle timeout of 5 s (NAME:START:DURATION after
# the subscription); that subscriber then has no other outage. RELAY_ARGS go to relay_receiver.py.
# Policies of the relay: fifo, prio, prio-to, alert. The device runs the proposed scheduler with
# the result probe; the images ao and aq are the same build under two names, alternated so that
# every run installs an image and restarts the device. Pause between runs with the file `pause`.
S=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/alert-state; mkdir -p $S
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
P=vendor/venv/bin/python
PREFIX=${PREFIX:-rl1}
SUBS="--receiver-arg=--sub --receiver-arg=ctrl:f128:results --receiver-arg=--sub --receiver-arg=mon:f128:all --receiver-arg=--sub --receiver-arg=qa:f512:all --receiver-arg=--sub --receiver-arg=arch:trace:all"
rounds=$1; shift
elf() { python3 -c "import sys;print(open(sys.argv[1],'rb').read()[176:208].hex())" $1; }
current() { grep -o "current=[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d= -f2; }
for attempt in 1 2; do for r in $(seq 1 $rounds); do for pol in "$@"; do
  n=$PREFIX-$pol-r$r
  grep -q '"status": "complete"' results/$n/manifest.json 2>/dev/null && [ -s results/$n/serial.log ] && continue
  while [ -f $S/pause ]; do touch $S/paused; read -t 5 <> <(:); done; rm -f $S/paused
  # LATEST=HHMM: no run starts after it, so that every run is in daylight
  [ -n "$LATEST" ] && [ "$(date +%H%M)" -ge "$LATEST" ] && { echo "$(date +%T) stopped: after $LATEST"; break 3; }
  F=$PWD/vendor/build-draft21-s3-ao-mus-alert/aiot_quic.bin
  [ "$(elf $F)" = "$(current)" ] && F=$PWD/vendor/build-draft21-s3-aq-mus-alert/aiot_quic.bin
  rm -rf results/$n
  first=""; [ "$FIRST" = impaired ] && first="--jitter-ms 5 --ge-loss 1:30:50:0 $(python3 tools/outages.py $((r*10+9)))"
  long=""; for l in $LONG; do long="$long --receiver-arg=--long --receiver-arg=$l"; done
  for x in $RELAY_ARGS; do long="$long --receiver-arg=$x"; done
  $P $PWD/remote_ota_trial.py --name $n --firmware $F --receiver relay_receiver.py \
    --receiver-arg=--policy --receiver-arg=$pol $SUBS $long \
    --receiver-arg=--seed --receiver-arg=$r --receiver-arg=--trace-offset --receiver-arg=$(( (r-1)*130 )) \
    --shaped --delay-ms 5 --rate-kbit 512 $first --timeout 330 --no-restore --netlog > $S/trial-$n.log 2>&1
  st=$(grep -o '"status": "[a-z]*"' results/$n/manifest.json 2>/dev/null)
  ev=$(grep -c MOQ_PASSAGE results/$n/serial.log 2>/dev/null)
  echo "$(date +%T) $n $st passages $ev"
  if [ "${ev:-0}" -lt 8 ]; then
    mv results/$n results/rejected-$n-$(date +%s); touch $S/pause; echo "$(date +%T) PAUSED: only ${ev:-0} passages in $n"
  fi
  burst=$(python3 tools/longest_burst.py results/$n/serial.log 2>/dev/null)
  if [ "${burst:-0}" -gt 25 ]; then
    mv results/$n results/rejected-$n-$(date +%s); touch $S/pause; echo "$(date +%T) PAUSED: a burst of $burst passages in $n (light?)"
  fi
done; done; done
echo CAMPAIGN_DONE
