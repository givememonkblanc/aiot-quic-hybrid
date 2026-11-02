#!/usr/bin/env bash
# Alert campaign on the real turntable: every result that reports a passage must reach the
# receiver at once and without loss. usage: [PREFIX=al-v1] [IMAGES=al] campaign_alert.sh <rounds> <method>...
# PREFIX names the runs, IMAGES the build of each method, vendor/build-draft21-s3-<IMAGES>-<method>.
# The methods alternate within a link, so the device never runs the image of the next run and
# the over-the-air update always happens. Pause between runs with the file `pause`.
# Links: shaped to 128 or 512 kbit/s, the variable trace, and the Wi-Fi as it is. The added
# one-way delay is 5 ms: the receiver stands next to the line.
# LINKS="f128 f512 fwifi trace" are the links of a factory Wi-Fi: burst loss (Gilbert-Elliott,
# p 1%, r 30%, half lost in the bad state: 1.6% on average, bursts of 3.3 packets), a delay of
# 5 ms with 5 ms of normal jitter, and short outages of 0.2-2 s every 20 s on average, the same
# for every method of a round (tools/outages.py). f128 and f512 are shaped to 128 and 512
# kbit/s, fwifi is the real Wi-Fi with these impairments added, and trace replays at 256 kbit/s
# the delay and loss measured on the Wi-Fi link of the device (TRACE, tools/ping_trace.py), a
# different 130 s of it in each round.
S=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/alert-state; mkdir -p $S
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
P=vendor/venv/bin/python
TRACE="0:192,20:64,45:0,57:128,80:48,100:192"
PREFIX=${PREFIX:-al-v1}; IMAGES=${IMAGES:-al}; LINKS=${LINKS:-128 512 wifi var}
TRACE_CSV=${TRACE_CSV:-$HOME/data/aiot_quic_results/wifi_trace/trace.csv}
IMPAIR="--jitter-ms 5 --ge-loss 1:30:50:0"
rounds=$1; shift
elf() { python3 -c "import sys;print(open(sys.argv[1],'rb').read()[176:208].hex())" $1; }
current() { grep -o "current=[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d= -f2; }
for attempt in 1 2; do for r in $(seq 1 $rounds); do for link in $LINKS; do for m in "$@"; do
  n=$PREFIX-$m-$link-r$r
  grep -q '"status": "complete"' results/$n/manifest.json 2>/dev/null && [ -s results/$n/serial.log ] && continue
  while [ -f $S/pause ]; do touch $S/paused; read -t 5 <> <(:); done; rm -f $S/paused
  F=$PWD/vendor/build-draft21-s3-$IMAGES-$m/aiot_quic.bin
  # The device runs this image already, so no update would happen: the run comes later.
  [ "$(elf $F)" = "$(current)" ] && { echo "$(date +%T) $n later: image already on the device"; continue; }
  rm -rf results/$n
  li=0; for x in $LINKS; do li=$((li+1)); [ $x = $link ] && break; done
  outs=$(python3 tools/outages.py $((r*10+li)) ${OUTGAP:+--gap $OUTGAP})  # OUTGAP: mean seconds between outages
  case $link in
    var) opt="--shaped --delay-ms 5 --rate-kbit 192 --rate-trace $TRACE";;
    wifi) opt="";;
    f128) opt="--shaped --delay-ms 5 --rate-kbit 128 $IMPAIR $outs";;
    f512) opt="--shaped --delay-ms 5 --rate-kbit 512 $IMPAIR $outs";;
    fwifi) opt="--shaped --delay-ms 5 --rate-kbit 20000 $IMPAIR $outs";;
    trace) opt="--shaped --rate-kbit 256 --delay-trace $TRACE_CSV --trace-offset $(( (r-1)*130 ))";;
    *) opt="--shaped --delay-ms 5 --rate-kbit $link";;
  esac
  $P $PWD/remote_ota_trial.py --name $n --firmware $F --receiver receiver_draft21.py \
    --receiver-arg=--evidence --receiver-arg=push --timeout 330 --no-restore --netlog $opt > $S/trial-$n.log 2>&1
  st=$(grep -o '"status": "[a-z]*"' results/$n/manifest.json 2>/dev/null)
  ev=$(grep -c MOQ_PASSAGE results/$n/serial.log 2>/dev/null)
  echo "$(date +%T) $n $st passages $ev"
  # The camera must see the plate: few passages mean it moved or the turntable stopped.
  # The run is set aside and repeated after the pause.
  if [ "${ev:-0}" -lt 8 ]; then
    mv results/$n results/rejected-$n-$(date +%s); touch $S/pause; echo "$(date +%T) PAUSED: only ${ev:-0} passages in $n"
  fi
  # In the dark the counter reports false passages: the bursts of the rotations (16-19 screws,
  # 15 s apart) merge into one. A burst of more than 25 sets the run aside and pauses.
  burst=$(python3 tools/longest_burst.py results/$n/serial.log 2>/dev/null)
  if [ "${burst:-0}" -gt 25 ]; then
    mv results/$n results/rejected-$n-$(date +%s); touch $S/pause; echo "$(date +%T) PAUSED: a burst of $burst passages in $n (light?)"
  fi
done; done; done; done
echo CAMPAIGN_DONE
