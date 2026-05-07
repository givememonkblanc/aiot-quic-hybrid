# QUIC performance evaluation workflow

This workflow combines:

- `tshark` packet capture for wire-level throughput and packet-rate evidence
- `telemetry.csv` analysis for QUIC congestion and latency stability evidence

## Metrics

Recommended core metrics:

1. **Application goodput**
   - Derived from `telemetry.csv` `payload_len`
   - Best for “how much useful data made progress?”
2. **Congestion indicators**
   - Derived from `rtt_ms`, `loss_percent`, `tx_queue_depth`, `datagram_budget`, `stream_window_available`, `quic_ready`
   - Use `congestion_event_ratio` plus RTT/loss summaries
3. **Latency stability**
   - Derived from RTT percentiles and `rtt_jitter_mean_abs_delta`
   - Best extra metric beyond throughput/congestion
4. **Wire throughput**
   - Derived from packet capture exported with `tshark`
   - Useful for checking wire cost vs. application goodput

## 1) Capture QUIC traffic

```bash
bash experiments/capture_helpers/capture_quic_pcap.sh <interface> <udp_port> [output_pcapng]
```

Example:

```bash
bash experiments/capture_helpers/capture_quic_pcap.sh wlan0 4445 \
  experiments/runtime_server_files/pcap_runs/$(date +%Y%m%d-%H%M%S)/quic_capture.pcapng
```

## 2) Export packet CSV from the capture

```bash
bash experiments/capture_helpers/export_quic_packet_csv.sh <input_pcapng> <udp_port> <output_csv>
```

Example:

```bash
bash experiments/capture_helpers/export_quic_packet_csv.sh \
  experiments/runtime_server_files/pcap_runs/20260425-120000/quic_capture.pcapng \
  4445 \
  experiments/runtime_server_files/pcap_runs/20260425-120000/packets.csv
```

Exported CSV columns:

- `frame.time_epoch`
- `frame.len`
- `udp.length`
- `ip.src`
- `ip.dst`
- `udp.srcport`
- `udp.dstport`

## 3) Analyze telemetry and packet CSV together

```bash
python3 experiments/telemetry_analyzer/analyze_quic_perf.py \
  --telemetry experiments/runtime_server_files/live_matplotlib/<run_id>/telemetry.csv \
  --packets experiments/runtime_server_files/pcap_runs/<run_id>/packets.csv \
  --window-seconds 1.0 \
  --output-json experiments/runtime_server_files/perf_reports/<run_id>/summary.json \
  --window-csv experiments/runtime_server_files/perf_reports/<run_id>/windows.csv
```

Telemetry-only analysis also works:

```bash
python3 experiments/telemetry_analyzer/analyze_quic_perf.py \
  --telemetry experiments/runtime_server_files/live_matplotlib/<run_id>/telemetry.csv
```

## Output interpretation

- `app_goodput_avg_bps`: useful payload throughput from telemetry
- `app_goodput_peak_window_bps`: peak 1-second payload goodput
- `wire_throughput_avg_bps`: observed wire throughput from packet capture
- `congestion_event_ratio`: fraction of telemetry samples showing congestion pressure
- Current heuristic: counts a sample as congested when it shows loss, queue pressure >= 50%, or RTT inflation beyond 125% of baseline RTT
- `rtt_ms.p95`: tail latency under load
- `rtt_ms.jitter_mean_abs_delta`: latency stability indicator
- `queue_pressure_percent`: `tx_queue_depth / datagram_budget * 100`

## Notes

- `tshark` is required for the capture/export helpers.
- In this workspace, `tshark` was not installed during validation, so the shell helpers were syntax-checked but not executed end-to-end.
- The analyzer itself uses only Python standard library modules.
