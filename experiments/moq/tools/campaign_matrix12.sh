#!/usr/bin/env bash
# Method matrix of the mission-based MoQ evaluation (design: claude.ai/artifact/8zAcXa9uxWiMCDBx7rFFc7):
# every method in every situation, on the real turntable, three rotations (213 s) per run.
#
# Methods (device image from tools/build_matrix.sh + server):
#   fifo     results at the priority of the images      | our relay, arrival order
#   prio     results first, medium representation       | our relay, priority order (5.1.2)
#   prio-to  prio + image timeout D                     | our relay, priority + delivery timeout
#   abr      throughput-based representation + prio-to  | our relay, priority + delivery timeout
#   dtp      deadline-aware representation + prio-to    | our relay, priority + delivery timeout
#   bbr      prio-to over BBR                           | our relay, priority + delivery timeout, BBR
#   imq      prio-to device                             | imquic draft-21 relay
#   mqtt     prio device over MQTT 3.1.1/TLS            | mosquitto; first hop shapes TCP
#   mbm      mission-based (window, plan, probe)        | our relay, mission-based egress (alert)
# Situations (first hop = device link, shaped on the host; second hops as in every run):
#   s1 nominal      512 kbit/s, 5 ms
#   s2 thin         128 kbit/s
#   s3 lossy        512 + Gilbert-Elliott burst loss and 5 ms jitter
#   s4 dev-outage   512 + two 10 s outages of the device link
#   s5 sub-outage   512 + outages of 10 s (mon) and 15 s (qa) that end their sessions
#   s6 contention   512 + the Orin on the same 2.4 GHz channel sends 15 Mbit/s of UDP
#   s7 compound     s2 + s3 + s4 + s6
# Subscribers: ctrl (results, f128), mon (all, f128), qa (all, f512), arch (all, trace),
# remote (all, the laptop over the Internet).
#
# usage: [PREFIX=mx1] [LATEST=HHMM] [SITS="s1 .."] [METHODS=".."] campaign_matrix12.sh <first round> <last round>
# (campaign_matrix.sh with dtp and bbr, and a check of the light: a run whose camera frames have a
# median above DARK bytes was made in the dark; it is rejected and the campaign pauses)
# campaign_matrix4.sh: campaign_matrix3.sh with the baselines that remove two advantages of mbm,
# under the same prefix, seeds and schedules (tools/build_matrix.sh):
#   prio-to-tau      prio-to that knows the budget: image deadline 1.2 s from the capture, relay D = tau
#   prio-to-low      prio-to at the low representation (small images)
#   prio-to-low-tau  both (sends no image: see build_matrix.sh)
# campaign_matrix12.sh: campaign_matrix11.sh with mbm-floor-pred and mbm-pred (AIOT_MOQT_MUS_PREDICT,
# tools/build_matrix.sh), relay policy alert.
# campaign_matrix11.sh: campaign_matrix10.sh with mbm-floor-wmm and prio-to-low-wmm (AIOT_MOQT_WMM:
# result and acknowledgement packets in the Wi-Fi voice access category)
# campaign_matrix10.sh: campaign_matrix9.sh with the model-size pilot: mbm-floor-<model> and
# prio-to-low-<model> for the models e1 and v4 (tools/build_models.sh)
# campaign_matrix9.sh: campaign_matrix8.sh with mbm-floor-bdp (relay --window-bdp) and the situation
# h500, 128 kbit/s with 480 ms of one-way delay on the device link (round trip about 500 ms)
# campaign_matrix8.sh: campaign_matrix7.sh with mbm-floor-fix16k and mbm-floor-fix1k6, mbm-floor with a
# constant evidence window on the device and the relay (build_matrix.sh)
# campaign_matrix7.sh: campaign_matrix6.sh with the situations of the rate sweep, r256 (256 kbit/s)
# and r1m (1 Mbit/s); s2 (128) and s1 (512) complete the sweep
# campaign_matrix6.sh: campaign_matrix5.sh and mbm-floor, mbm whose plan values every representation
# alike (build_matrix.sh), with the relay of mbm
# campaign_matrix5.sh: with prio-to-low-d5 (small images, D = 5 s; build_matrix.sh) in place of prio-to-low-tau
# Within a round the situations and the methods within a situation are rotated by the round, so
# that drift over the day spreads over all methods. Every situation and seed of a round is the same
# for all methods. A run with fewer than MINPASS passages, or a burst of more than 25 passages, is
# rejected and made again in a second pass. A failed run leaves its image unconfirmed; the device
# returns to its previous image by itself, and the campaign waits for that. Pause with the file
# `pause`; two failures in a row pause the campaign.
S=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/alert-state; mkdir -p $S
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
P=vendor/venv/bin/python
PREFIX=${PREFIX:-mx1}
SITS=${SITS:-"s1 s2 s3 s4 s5 s6 s7"}
METHODS=${METHODS:-"prio-to-tau prio-to-low prio-to-low-d5 mbm mbm-floor"}
MINPASS=${MINPASS:-25}
DARK=${DARK:-150000}
SPAN=213
ORIN=${ORIN:-orin}
LOAD=${LOAD:-15M}
SUBS="--receiver-arg=--sub --receiver-arg=ctrl:f128:results --receiver-arg=--sub --receiver-arg=mon:f128:all --receiver-arg=--sub --receiver-arg=qa:f512:all --receiver-arg=--sub --receiver-arg=arch:trace:all --receiver-arg=--sub --receiver-arg=remote:wan:all"
first=$1; last=$2
elf() { python3 -c "import sys;print(open(sys.argv[1],'rb').read()[176:208].hex())" $1; }
current() { grep -o "current=[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d= -f2; }
last_download() { grep -o "/images/[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d/ -f3; }
recovered() { tail -n +$1 ../ota/results/service.log | awk '/\/images\//{ok=0} /\/next.*pending=0/{ok=1} END{exit !ok}'; }
rot() { local k=$1; shift; local a=($@); local n=${#a[@]}; for i in $(seq 0 $((n-1))); do echo -n "${a[$(( (i+k) % n ))]} "; done; }
# seeded schedules of a round: first-hop outages (s4, s7) and subscriber outages (s5)
devout() { python3 -c "import random;r=random.Random($1*31+4);print(f'--outage={r.uniform(20,80):.1f}:10 --outage={r.uniform(120,180):.1f}:10')"; }
subout() { python3 -c "import random;r=random.Random($1*37+5);print(f'--receiver-arg=--long --receiver-arg=mon:{r.uniform(20,80):.1f}:10 --receiver-arg=--long --receiver-arg=qa:{r.uniform(110,170):.1f}:15')"; }
load_on() { ssh -o ConnectTimeout=8 $ORIN "pkill -x iperf3; nohup iperf3 -c 192.168.0.38 -p 5299 -u -b $LOAD -l 1200 -t 480 --forceflush > /tmp/mx-load.log 2>&1 &" ; }
load_off() { ssh -o ConnectTimeout=8 $ORIN "pkill -x iperf3" ; }
pgrep -f "iperf3 -s -p 5299" >/dev/null || iperf3 -s -p 5299 -D
fails=0
for pass in 1 2; do for r in $(seq $first $last); do for sit in $(rot $((r-1)) $SITS); do for m in $(rot $((r-1)) $METHODS); do
  n=$PREFIX-$sit-$m-r$r
  grep -q '"status": "complete"' results/$n/manifest.json 2>/dev/null && [ -s results/$n/serial.log ] && continue
  while [ -f $S/pause ]; do touch $S/paused; read -t 5 <> <(:); done; rm -f $S/paused
  [ -n "$LATEST" ] && [ "$(date +%H%M)" -ge "$LATEST" ] && { echo "$(date +%T) stopped: after $LATEST"; break 4; }
  case $m in
    fifo|prio|prio-to) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=$m";;
    abr|dtp) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=prio-to";;
    prio-to-low|prio-to-low-d5|prio-to-low-e1|prio-to-low-v4|prio-to-low-wmm) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=prio-to";;
    prio-to-tau|prio-to-low-tau) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=prio-to --receiver-arg=--image-timeout-ms --receiver-arg=200";;
    bbr) img=bbr; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=prio-to --receiver-arg=--cc --receiver-arg=bbr";;
    mbm|mbm-floor|mbm-floor-e1|mbm-floor-v4|mbm-floor-wmm|mbm-floor-pred|mbm-pred) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=alert";;
    mbm-floor-bdp) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=alert --receiver-arg=--window-bdp";;
    mbm-floor-fix16k) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=alert --receiver-arg=--fixed-window --receiver-arg=16384";;
    mbm-floor-fix1k6) img=$m; recv=relay_receiver.py; extra="--receiver-arg=--policy --receiver-arg=alert --receiver-arg=--fixed-window --receiver-arg=1600";;
    imq) img=prio-to; recv=baseline_receiver.py; extra="--receiver-arg=--server --receiver-arg=imquic";;
    mqtt) img=mqtt; recv=baseline_receiver.py; extra="--receiver-arg=--server --receiver-arg=mqtt --ip-proto tcp";;
    *) echo "unknown method $m"; exit 1;;
  esac
  case $sit in
    s1) hop="--rate-kbit 512"; load=0;;
    s2) hop="--rate-kbit 128"; load=0;;
    s3) hop="--rate-kbit 512 --jitter-ms 5 --ge-loss 1:30:50:0"; load=0;;
    s4) hop="--rate-kbit 512 $(devout $r)"; load=0;;
    s5) hop="--rate-kbit 512 $(subout $r)"; load=0;;
    s6) hop="--rate-kbit 512"; load=1;;
    s7) hop="--rate-kbit 128 --jitter-ms 5 --ge-loss 1:30:50:0 $(devout $r)"; load=1;;
    r256) hop="--rate-kbit 256"; load=0;;
    h500) hop="--rate-kbit 128 --delay-ms 480"; load=0;;
    r1m) hop="--rate-kbit 1000"; load=0;;
    *) echo "unknown situation $sit"; exit 1;;
  esac
  F=$PWD/vendor/build-draft21-s3-mx-a-$img/aiot_quic.bin
  case "$(elf $F)" in "$(current)"|"$(last_download)") F=$PWD/vendor/build-draft21-s3-mx-b-$img/aiot_quic.bin;; esac
  rm -rf results/$n
  from=$(( $(wc -l < ../ota/results/service.log) + 1 ))
  [ $load = 1 ] && load_on
  $P $PWD/remote_ota_trial.py --name $n --firmware $F --receiver $recv $extra $SUBS \
    --receiver-arg=--seed --receiver-arg=$r --receiver-arg=--trace-offset --receiver-arg=$(( (r-1)*230 )) \
    --receiver-arg=--span --receiver-arg=$SPAN \
    --shaped --delay-ms 5 $hop --timeout 440 --no-restore --netlog > $S/trial-$n.log 2>&1
  [ $load = 1 ] && load_off
  python3 -c "import json,sys;json.dump(dict(prefix='$PREFIX',situation='$sit',method='$m',round=$r,image='$(basename $(dirname $F))',first_hop='$hop'.split(),load='$LOAD' if $load else None,pass_=$pass),open(sys.argv[1],'w'),indent=1)" results/$n/matrix.json 2>/dev/null
  st=$(grep -o '"status": "[a-z]*"' results/$n/manifest.json 2>/dev/null)
  ev=$(grep -c MOQ_PASSAGE results/$n/serial.log 2>/dev/null)
  burst=$(python3 tools/longest_burst.py results/$n/serial.log 2>/dev/null)
  frame=$(python3 tools/frame_light.py results/$n/serial.log 2>/dev/null)
  echo "$(date +%T) $n $st passages ${ev:-0} burst ${burst:-0} frame ${frame:-0}"
  if [ "${frame:-0}" -gt $DARK ]; then
    mv results/$n results/rejected-$n-$(date +%s); touch $S/pause
    echo "$(date +%T) PAUSED: $n was made in the dark (median frame ${frame} bytes); turn the light on and remove $S/pause"
    continue
  fi
  if [ "${ev:-0}" -lt $MINPASS ] || [ "${burst:-0}" -gt 25 ]; then
    mv results/$n results/rejected-$n-$(date +%s); fails=$((fails+1)); t=0
    until recovered $from || [ $t -ge 600 ]; do read -t 10 <> <(:); t=$((t+10)); done
    if [ $fails -ge 2 ] || ! recovered $from; then
      touch $S/pause; echo "$(date +%T) PAUSED: $n rejected (failures $fails)"
    else
      echo "$(date +%T) rejected $n; the device returned to $(current | cut -c1-12)"
    fi
    continue
  fi
  fails=0
done; done; done; done
echo CAMPAIGN_DONE
