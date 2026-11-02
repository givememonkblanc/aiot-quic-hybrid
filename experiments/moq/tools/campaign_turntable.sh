#!/usr/bin/env bash
# Real turntable and camera, device reached over Wi-Fi only. usage: campaign-tt.sh <rounds> <method>...
# Each run installs the a or b image of its method, whichever the device does not run, so that
# the over-the-air update always happens. Pause between runs with the file `pause`.
S=/home/ryzen395/aiot_quic_baseline/experiments/moq/vendor/campaign-state; mkdir -p $S
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
P=vendor/venv/bin/python
TRACE="0:192,20:64,45:0,57:128,80:48,100:192"
rounds=$1; shift
elf() { python3 -c "import sys;print(open(sys.argv[1],'rb').read()[176:208].hex())" $1; }
current() { grep -o "current=[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d= -f2; }
for attempt in 1 2; do for r in $(seq 1 $rounds); do for m in "$@"; do for link in 64 96 128 var; do
  n=tt-v1-$m-$link-r$r
  grep -q '"status": "complete"' results/$n/manifest.json 2>/dev/null && [ -s results/$n/serial.log ] && continue
  while [ -f $S/pause ]; do touch $S/paused; read -t 5 <> <(:); done; rm -f $S/paused
  F=$PWD/vendor/build-draft21-s3-rt-$m-a/aiot_quic.bin
  [ "$(elf $F)" = "$(current)" ] && F=$PWD/vendor/build-draft21-s3-rt-$m-b/aiot_quic.bin
  rm -rf results/$n
  if [ $link = var ]; then opt="--rate-kbit 192 --rate-trace $TRACE"; else opt="--rate-kbit $link"; fi
  $P $PWD/remote_ota_trial.py --name $n --firmware $F --receiver receiver_draft21.py \
    --receiver-arg=--evidence --receiver-arg=push --timeout 330 --shaped --delay-ms 50 --no-restore --netlog $opt > $S/trial-$n.log 2>&1
  st=$(grep -o '"status": "[a-z]*"' results/$n/manifest.json 2>/dev/null)
  echo "$(date +%T) $n $st $(wc -l < results/$n/serial.log 2>/dev/null) lines"
done; done; done; done
echo CAMPAIGN_DONE
