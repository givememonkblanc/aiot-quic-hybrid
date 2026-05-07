#!/bin/bash
cd /home/ryzen395/aiot_quic_baseline/host_receiver
mkdir -p qlog_dir
./picoquic/picoquicdemo \
  -p 14567 \
  -c /home/ryzen395/aiot_quic_baseline/host_receiver/certs/cert.pem \
  -k /home/ryzen395/aiot_quic_baseline/host_receiver/certs/key.pem \
  -a mqtt \
  -q qlog_dir \
  -L 2>&1
