#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  printf 'Usage: %s <interface> <udp_port> [output_pcapng]\n' "$0" >&2
  exit 1
fi

INTERFACE="$1"
UDP_PORT="$2"
OUTPUT_PATH="${3:-experiments/runtime_server_files/pcap_runs/$(date +%Y%m%d-%H%M%S)/quic_capture.pcapng}"

mkdir -p "$(dirname "$OUTPUT_PATH")"

printf 'Capturing QUIC/UDP traffic on interface=%s udp_port=%s -> %s\n' "$INTERFACE" "$UDP_PORT" "$OUTPUT_PATH"
printf 'Stop with Ctrl+C when enough traffic has been collected.\n'

exec tshark -i "$INTERFACE" -f "udp port $UDP_PORT" -w "$OUTPUT_PATH"
