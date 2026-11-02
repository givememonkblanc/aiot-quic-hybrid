#!/usr/bin/env bash
# Sessions of full-resolution frames of the turntable for the screw detector, each of about
# three rotations. usage: collect_turntable.sh <name>... ; waits for the paused campaign.
cd /home/ryzen395/aiot_quic_baseline/experiments/moq
S=vendor/alert-state
until [ -f $S/paused ]; do read -t 5 <> <(:); done
elf() { python3 -c "import sys;print(open(sys.argv[1],'rb').read()[176:208].hex())" $1; }
current() { grep -o "current=[0-9a-f]*" ../ota/results/service.log | tail -1 | cut -d= -f2; }
for n in "$@"; do
  [ -f results/$n/manifest.json ] && continue
  # two copies of the image, so that the device always takes an update
  F=$PWD/vendor/build-draft21-s3-turntable-blobs/aiot_quic.bin
  [ "$(elf $F)" = "$(current)" ] && F=$PWD/vendor/build-draft21-s3-turntable-blobs-b/aiot_quic.bin
  vendor/venv/bin/python $PWD/remote_ota_trial.py --name $n --firmware $F --receiver receiver_draft21.py \
    --receiver-arg=--evidence --receiver-arg=push --receiver-arg=--image-track --receiver-arg=preview/jpeg \
    --receiver-arg=--seconds --receiver-arg=200 --timeout 360 --shaped --delay-ms 20 --no-restore --netlog \
    --rate-kbit 4096 > $S/collect-$n.log 2>&1
  echo "$(date +%T) $n $(grep -o '"status": "[a-z]*"' results/$n/manifest.json) frames $(ls results/$n | grep -c '^frame-')"
done
echo COLLECT_DONE
