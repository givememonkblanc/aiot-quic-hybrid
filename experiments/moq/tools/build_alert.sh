#!/usr/bin/env bash
# Image of one method for the alert campaign: every method sends up to four results at once,
# so that results do not queue on the device behind a slow acknowledgement.
m=$1
case $m in
  mus-alert) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=100";;
  mus-1000) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=1000";;
  mus-nobound) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=0";;
  *) base=$m; extra="";;
esac
common="-DAIOT_MOQT_REPLAY_INPUT=OFF -DAIOT_MOQT_PASSAGE_SYNTHETIC=OFF -DAIOT_CAMERA_JPEG_QUALITY=20 -DAIOT_CAMERA_XCLK_HZ=10000000 -DAIOT_NETLOG_HOST=192.168.0.38 -DAIOT_AI_DARK_BLOBS=ON -DAIOT_MOQT_RESULT_SLOTS=4"
echo "al-$m $(/home/ryzen395/aiot_quic_baseline/experiments/moq/tools/build_variant.sh $base al-$m $common $extra)"
