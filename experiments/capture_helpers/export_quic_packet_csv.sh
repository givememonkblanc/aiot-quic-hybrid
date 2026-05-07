#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  printf 'Usage: %s <input_pcapng> <udp_port> <output_csv>\n' "$0" >&2
  exit 1
fi

INPUT_PCAP="$1"
UDP_PORT="$2"
OUTPUT_CSV="$3"

mkdir -p "$(dirname "$OUTPUT_CSV")"

tshark -r "$INPUT_PCAP" \
  -Y "udp.port == $UDP_PORT" \
  -T fields \
  -E header=y \
  -E separator=, \
  -E quote=d \
  -e frame.time_epoch \
  -e frame.len \
  -e udp.length \
  -e ip.src \
  -e ip.dst \
  -e udp.srcport \
  -e udp.dstport \
  > "$OUTPUT_CSV"

printf 'Exported packet CSV to %s\n' "$OUTPUT_CSV"
