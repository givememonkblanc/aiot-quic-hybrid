#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean
from typing import Iterable


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct / 100.0
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return ordered[lower]
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def safe_mean(values: Iterable[float]) -> float:
    values = list(values)
    return mean(values) if values else 0.0


def bool_from_cell(value: str) -> bool:
    return str(value).strip().lower() in {"1", "true", "yes", "y"}


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def window_sums(samples: list[tuple[float, int]], window_seconds: float) -> list[dict[str, float]]:
    if not samples:
        return []
    start_time = samples[0][0]
    buckets: dict[int, int] = {}
    for timestamp, byte_count in samples:
        index = int((timestamp - start_time) / window_seconds)
        buckets[index] = buckets.get(index, 0) + byte_count

    rows: list[dict[str, float]] = []
    for index in sorted(buckets):
        total_bytes = buckets[index]
        rows.append(
            {
                "window_index": index,
                "window_start_s": round(index * window_seconds, 6),
                "window_end_s": round((index + 1) * window_seconds, 6),
                "bytes": total_bytes,
                "bits_per_second": (total_bytes * 8.0) / window_seconds,
            }
        )
    return rows


def summarize_telemetry(rows: list[dict[str, str]], window_seconds: float) -> dict[str, object]:
    timestamps = [int(row["timestamp_us"]) / 1_000_000.0 for row in rows if row.get("timestamp_us")]
    payloads = [int(row.get("payload_len", 0) or 0) for row in rows]
    rtts = [float(row.get("rtt_ms", 0) or 0) for row in rows]
    losses = [float(row.get("loss_percent", 0) or 0) for row in rows]
    queue_depths = [float(row.get("tx_queue_depth", 0) or 0) for row in rows]
    budgets = [float(row.get("datagram_budget", 0) or 0) for row in rows]
    stream_windows = [bool_from_cell(row.get("stream_window_available", "0")) for row in rows]
    quic_ready_flags = [bool_from_cell(row.get("quic_ready", "0")) for row in rows]

    duration_seconds = max(timestamps) - min(timestamps) if len(timestamps) >= 2 else 0.0
    total_payload_bytes = sum(payloads)
    payload_samples = list(zip(timestamps, payloads))
    payload_windows = window_sums(payload_samples, window_seconds)

    queue_pressures = [
        (depth / budget) * 100.0
        for depth, budget in zip(queue_depths, budgets)
        if budget > 0
    ]
    rtt_deltas = [abs(curr - prev) for prev, curr in zip(rtts, rtts[1:])]
    nonzero_rtts = [value for value in rtts if value > 0]
    baseline_rtt = min(nonzero_rtts, default=0.0)

    congestion_events = 0
    for row in rows:
        loss = float(row.get("loss_percent", 0) or 0)
        queue_depth = float(row.get("tx_queue_depth", 0) or 0)
        budget = float(row.get("datagram_budget", 0) or 0)
        rtt_ms = float(row.get("rtt_ms", 0) or 0)
        queue_pressure = ((queue_depth / budget) * 100.0) if budget > 0 else 0.0
        rtt_inflated = baseline_rtt > 0 and rtt_ms >= (baseline_rtt * 1.25)
        if loss > 0 or queue_pressure >= 50.0 or rtt_inflated:
            congestion_events += 1

    return {
        "sample_count": len(rows),
        "duration_seconds": duration_seconds,
        "total_payload_bytes": total_payload_bytes,
        "app_goodput_avg_bps": (total_payload_bytes * 8.0 / duration_seconds) if duration_seconds > 0 else 0.0,
        "app_goodput_peak_window_bps": max((row["bits_per_second"] for row in payload_windows), default=0.0),
        "rtt_ms": {
            "avg": safe_mean(rtts),
            "p50": percentile(rtts, 50),
            "p95": percentile(rtts, 95),
            "max": max(rtts, default=0.0),
            "jitter_mean_abs_delta": safe_mean(rtt_deltas),
        },
        "loss_percent": {
            "avg": safe_mean(losses),
            "p95": percentile(losses, 95),
            "max": max(losses, default=0.0),
        },
        "queue_depth": {
            "avg": safe_mean(queue_depths),
            "p95": percentile(queue_depths, 95),
            "max": max(queue_depths, default=0.0),
        },
        "queue_pressure_percent": {
            "avg": safe_mean(queue_pressures),
            "p95": percentile(queue_pressures, 95),
            "max": max(queue_pressures, default=0.0),
        },
        "quic_ready_ratio": (sum(1 for flag in quic_ready_flags if flag) / len(quic_ready_flags)) if quic_ready_flags else 0.0,
        "stream_blocked_ratio": (sum(1 for flag in stream_windows if not flag) / len(stream_windows)) if stream_windows else 0.0,
        "congestion_event_ratio": (congestion_events / len(rows)) if rows else 0.0,
        "window_series": payload_windows,
    }


def summarize_packets(rows: list[dict[str, str]], window_seconds: float) -> dict[str, object]:
    timestamps = [float(row["frame.time_epoch"]) for row in rows if row.get("frame.time_epoch")]
    frame_lengths = [int(row.get("frame.len", 0) or 0) for row in rows]
    udp_lengths = [int(row.get("udp.length", 0) or 0) for row in rows]
    total_bytes = sum(frame_lengths) if any(frame_lengths) else sum(udp_lengths)
    duration_seconds = max(timestamps) - min(timestamps) if len(timestamps) >= 2 else 0.0
    packet_samples = list(zip(timestamps, frame_lengths if any(frame_lengths) else udp_lengths))
    packet_windows = window_sums(packet_samples, window_seconds)
    packet_count_windows = window_sums([(timestamp, 1) for timestamp in timestamps], window_seconds)
    inter_arrivals_ms = [
        (curr - prev) * 1000.0 for prev, curr in zip(timestamps, timestamps[1:])
    ]

    return {
        "packet_count": len(rows),
        "duration_seconds": duration_seconds,
        "capture_total_bytes": total_bytes,
        "wire_throughput_avg_bps": (total_bytes * 8.0 / duration_seconds) if duration_seconds > 0 else 0.0,
        "wire_throughput_peak_window_bps": max((row["bits_per_second"] for row in packet_windows), default=0.0),
        "packet_rate_avg_pps": (len(rows) / duration_seconds) if duration_seconds > 0 else 0.0,
        "packet_rate_peak_window_pps": max((row["bits_per_second"] / 8.0 for row in packet_count_windows), default=0.0),
        "inter_arrival_ms": {
            "avg": safe_mean(inter_arrivals_ms),
            "p95": percentile(inter_arrivals_ms, 95),
            "max": max(inter_arrivals_ms, default=0.0),
        },
        "window_series": packet_windows,
    }


def write_window_csv(path: Path, telemetry_windows: list[dict[str, float]], packet_windows: list[dict[str, float]]) -> None:
    merged: dict[int, dict[str, float]] = {}

    for row in telemetry_windows:
        index = int(row["window_index"])
        merged.setdefault(index, {}).update(
            {
                "window_index": index,
                "window_start_s": row["window_start_s"],
                "window_end_s": row["window_end_s"],
                "telemetry_bytes": row["bytes"],
                "telemetry_bps": row["bits_per_second"],
            }
        )

    for row in packet_windows:
        index = int(row["window_index"])
        merged.setdefault(index, {}).update(
            {
                "window_index": index,
                "window_start_s": row["window_start_s"],
                "window_end_s": row["window_end_s"],
                "packet_bytes": row["bytes"],
                "packet_bps": row["bits_per_second"],
            }
        )

    fieldnames = [
        "window_index",
        "window_start_s",
        "window_end_s",
        "telemetry_bytes",
        "telemetry_bps",
        "packet_bytes",
        "packet_bps",
    ]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for index in sorted(merged):
            writer.writerow(merged[index])


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze QUIC telemetry.csv and optional tshark packet CSV exports."
    )
    parser.add_argument("--telemetry", type=Path, help="Path to telemetry.csv")
    parser.add_argument(
        "--packets",
        type=Path,
        help="Path to packet CSV exported by export_quic_packet_csv.sh",
    )
    parser.add_argument(
        "--window-seconds",
        type=float,
        default=1.0,
        help="Window size used for peak throughput calculations (default: 1.0)",
    )
    parser.add_argument("--output-json", type=Path, help="Optional JSON summary output path")
    parser.add_argument("--window-csv", type=Path, help="Optional merged windowed metrics CSV path")
    args = parser.parse_args()

    if not args.telemetry and not args.packets:
        parser.error("at least one of --telemetry or --packets is required")
    if args.window_seconds <= 0:
        parser.error("--window-seconds must be > 0")

    summary: dict[str, object] = {
        "metric_definitions": {
            "app_goodput_avg_bps": "Application payload throughput derived from telemetry payload_len.",
            "wire_throughput_avg_bps": "Observed capture throughput derived from tshark-exported packet/frame lengths.",
            "congestion_event_ratio": "Fraction of telemetry samples showing loss, >=50% queue pressure, or RTT inflation beyond 125% of baseline.",
            "rtt_jitter_mean_abs_delta": "Mean absolute delta between consecutive RTT samples; a latency-stability indicator.",
        }
    }

    telemetry_windows: list[dict[str, float]] = []
    packet_windows: list[dict[str, float]] = []

    if args.telemetry:
        telemetry_rows = load_csv(args.telemetry)
        telemetry_summary = summarize_telemetry(telemetry_rows, args.window_seconds)
        telemetry_windows = list(telemetry_summary.pop("window_series", []))
        summary["telemetry"] = telemetry_summary
        summary["telemetry_source"] = str(args.telemetry)

    if args.packets:
        packet_rows = load_csv(args.packets)
        packet_summary = summarize_packets(packet_rows, args.window_seconds)
        packet_windows = list(packet_summary.pop("window_series", []))
        summary["packets"] = packet_summary
        summary["packets_source"] = str(args.packets)

    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    if args.window_csv:
        args.window_csv.parent.mkdir(parents=True, exist_ok=True)
        write_window_csv(args.window_csv, telemetry_windows, packet_windows)

    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
