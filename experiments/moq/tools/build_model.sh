#!/usr/bin/env bash
# Image of one method with the trained screw model in place of the dark-region detector.
# usage: build_model.sh <method> <model dir> <name> [extra -D options]
m=$1; M=$2; name=$3; shift 3
case $m in
  mus-alert) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=100";;
  mus-1000) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=1000";;
  mus-nobound) base=mus; extra="-DAIOT_MOQT_MUS_MARGIN=1000 -DAIOT_MOQT_MUS_RISK=ON -DAIOT_MOQT_MUS_AHEAD=ON -DAIOT_MOQT_MUS_STATE_DELAY_MS=0";;
  *) base=$m; extra="";;
esac
common="-DAIOT_MOQT_REPLAY_INPUT=OFF -DAIOT_MOQT_PASSAGE_SYNTHETIC=OFF -DAIOT_CAMERA_JPEG_QUALITY=20 -DAIOT_CAMERA_XCLK_HZ=10000000 -DAIOT_NETLOG_HOST=192.168.0.38 -DAIOT_MOQT_RESULT_SLOTS=4 -DAIOT_AI_DARK_BLOBS=OFF -DAIOT_AI_ROI=ON -DAIOT_AI_ROI_SCALE=8 -DAIOT_CAMERA_MODEL_SOURCE=$M/ai_inference_vehicle_model.c -DAIOT_CAMERA_MODEL_METADATA=$M/screw_model_asset.h"
echo "$name $(/home/ryzen395/aiot_quic_baseline/experiments/moq/tools/build_variant.sh $base $name $common $extra "$@")"
