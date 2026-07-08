#!/usr/bin/env python3
"""Compare multiple experiment runs side by side.

Each run directory is expected to contain telemetry.csv and tx_events.csv as
produced by parse_aiot_monitor_log.py. Runs are given as label=path pairs so
baseline and upgraded captures can be compared in one table:

    python3 compare_runs.py \
        baseline-hybrid=runs/hybrid moqt-hybrid=runs_v2/hybrid \
        --markdown comparison.md --csv comparison.csv
"""
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


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def to_float(value: str | None) -> float:
    try:
        return float(value) if value not in (None, "") else 0.0
    except ValueError:
        return 0.0


def last_counter(rows: list[dict[str, str]], field: str) -> float:
    for row in reversed(rows):
        if row.get(field, "") != "":
            return to_float(row[field])
    return 0.0


def summarize_run(run_dir: Path) -> dict[str, float]:
    telemetry = load_csv(run_dir / "telemetry.csv")
    tx_events = load_csv(run_dir / "tx_events.csv")

    rtts = [to_float(row.get("rtt_ms")) for row in telemetry]
    # rtt_ms=0 means "no fresh RTT sample available", not a 0ms network path;
    # including zeros deflates averages (visible in the May stress captures).
    rtts_valid = [value for value in rtts if value > 0]
    timestamps = [to_float(row.get("timestamp_us")) / 1_000_000.0 for row in telemetry if row.get("timestamp_us")]

    stream_e2e = [to_float(row.get("e2e_us")) for row in tx_events if row.get("mode") == "stream" and row.get("ret") == "0"]
    dgram_e2e = [to_float(row.get("e2e_us")) for row in tx_events if row.get("mode") == "datagram" and row.get("ret") == "0"]
    all_e2e = stream_e2e + dgram_e2e
    qwaits = [to_float(row.get("qwait_us")) for row in tx_events if row.get("mode") in ("stream", "datagram")]

    expired_events = sum(1 for row in tx_events if row.get("mode") == "expired")
    evicted_events = sum(1 for row in tx_events if row.get("mode") == "evicted")

    return {
        "telemetry_samples": len(telemetry),
        "duration_s": (max(timestamps) - min(timestamps)) if len(timestamps) >= 2 else 0.0,
        "handshake_ms": last_counter(telemetry, "handshake_time_ms"),
        "rtt_avg_ms": safe_mean(rtts),
        "rtt_valid_ratio": (len(rtts_valid) / len(rtts)) if rtts else 0.0,
        "rtt_avg_valid_ms": safe_mean(rtts_valid),
        "rtt_p50_valid_ms": percentile(rtts_valid, 50),
        "rtt_p95_valid_ms": percentile(rtts_valid, 95),
        "rtt_p50_ms": percentile(rtts, 50),
        "rtt_p95_ms": percentile(rtts, 95),
        "freshness_avg_us": safe_mean(all_e2e),
        "freshness_p95_us": percentile(all_e2e, 95),
        "freshness_p99_us": percentile(all_e2e, 99),
        "stream_e2e_avg_us": safe_mean(stream_e2e),
        "datagram_e2e_avg_us": safe_mean(dgram_e2e),
        "queue_wait_avg_us": safe_mean(qwaits),
        "queue_wait_p95_us": percentile(qwaits, 95),
        "stream_send_ok": last_counter(telemetry, "stream_send_ok"),
        "stream_send_fail": last_counter(telemetry, "stream_send_fail"),
        "datagrams_sent": last_counter(telemetry, "datagrams_sent"),
        "datagrams_lost": last_counter(telemetry, "datagrams_lost"),
        "datagram_expired": max(last_counter(telemetry, "datagram_expired"), float(expired_events)),
        "datagram_evicted": max(last_counter(telemetry, "datagram_evicted"), float(evicted_events)),
        "route_queue_full": last_counter(telemetry, "route_queue_full_count"),
        "tx_stream_events": float(len(stream_e2e)),
        "tx_datagram_events": float(len(dgram_e2e)),
    }


METRIC_ORDER = [
    ("telemetry_samples", "Telemetry samples", "{:.0f}"),
    ("duration_s", "Duration (s)", "{:.1f}"),
    ("handshake_ms", "Handshake (ms)", "{:.0f}"),
    ("rtt_avg_ms", "RTT avg incl. zeros (ms)", "{:.1f}"),
    ("rtt_valid_ratio", "RTT valid sample ratio", "{:.2f}"),
    ("rtt_avg_valid_ms", "RTT avg (ms)", "{:.1f}"),
    ("rtt_p50_valid_ms", "RTT p50 (ms)", "{:.1f}"),
    ("rtt_p95_valid_ms", "RTT p95 (ms)", "{:.1f}"),
    ("freshness_avg_us", "Freshness avg (us)", "{:.0f}"),
    ("freshness_p95_us", "Freshness p95 (us)", "{:.0f}"),
    ("freshness_p99_us", "Freshness p99 (us)", "{:.0f}"),
    ("stream_e2e_avg_us", "Stream e2e avg (us)", "{:.0f}"),
    ("datagram_e2e_avg_us", "Datagram e2e avg (us)", "{:.0f}"),
    ("queue_wait_avg_us", "Queue wait avg (us)", "{:.0f}"),
    ("queue_wait_p95_us", "Queue wait p95 (us)", "{:.0f}"),
    ("stream_send_ok", "Stream send ok", "{:.0f}"),
    ("stream_send_fail", "Stream send fail", "{:.0f}"),
    ("datagrams_sent", "Datagrams sent", "{:.0f}"),
    ("datagrams_lost", "Datagrams lost (ACK-based)", "{:.0f}"),
    ("datagram_expired", "Datagrams expired (shed)", "{:.0f}"),
    ("datagram_evicted", "Datagrams evicted (shed)", "{:.0f}"),
    ("route_queue_full", "Route queue full count", "{:.0f}"),
    ("tx_stream_events", "TX events (stream)", "{:.0f}"),
    ("tx_datagram_events", "TX events (datagram)", "{:.0f}"),
]


def render_markdown(labels: list[str], summaries: dict[str, dict[str, float]]) -> str:
    lines = ["| Metric | " + " | ".join(labels) + " |",
             "|---|" + "---|" * len(labels)]
    for key, title, fmt in METRIC_ORDER:
        cells = [fmt.format(summaries[label].get(key, 0.0)) for label in labels]
        lines.append(f"| {title} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare experiment run directories side by side.")
    parser.add_argument("runs", nargs="+", help="label=path pairs (path contains telemetry.csv/tx_events.csv)")
    parser.add_argument("--markdown", type=Path, help="Optional markdown table output path")
    parser.add_argument("--csv", type=Path, help="Optional CSV output path")
    parser.add_argument("--json", type=Path, help="Optional JSON output path")
    args = parser.parse_args()

    labels: list[str] = []
    summaries: dict[str, dict[str, float]] = {}
    for spec in args.runs:
        if "=" in spec:
            label, path_str = spec.split("=", 1)
        else:
            label, path_str = Path(spec).name, spec
        run_dir = Path(path_str)
        if not (run_dir / "telemetry.csv").exists():
            parser.error(f"{run_dir} does not contain telemetry.csv")
        labels.append(label)
        summaries[label] = summarize_run(run_dir)

    table = render_markdown(labels, summaries)
    print(table)

    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(table, encoding="utf-8")
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(["metric"] + labels)
            for key, title, _ in METRIC_ORDER:
                writer.writerow([title] + [summaries[label].get(key, 0.0) for label in labels])
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(summaries, indent=2), encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
