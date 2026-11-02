#!/usr/bin/env bash
# Two copies (a, b) of the real-turntable image of one method, differing in the project version.
T=/home/ryzen395/aiot_quic_baseline/experiments/moq/tools
m=$1
case $m in
  mus-full) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=1000";;
  mus-norisk) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=800 -DAIOT_MOQT_MUS_RISK=OFF -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=1000";;
  mus-noahead) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=OFF -DAIOT_MOQT_MUS_STATE_DELAY_MS=1000";;
  mus) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=0";;
  *) base=$m; extra="";;
esac
common="-DAIOT_MOQT_REPLAY_INPUT=OFF -DAIOT_MOQT_PASSAGE_SYNTHETIC=OFF -DAIOT_CAMERA_JPEG_QUALITY=20 -DAIOT_CAMERA_XCLK_HZ=10000000 -DAIOT_NETLOG_HOST=192.168.0.38 -DAIOT_AI_DARK_BLOBS=ON"
for v in a b; do echo "rt-$m-$v $(/home/ryzen395/aiot_quic_baseline/experiments/moq/tools/build_variant.sh $base rt-$m-$v $common $extra)"; done
