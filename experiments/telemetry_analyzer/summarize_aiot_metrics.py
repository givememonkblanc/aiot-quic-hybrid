#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def to_float(row: dict[str, str], key: str) -> float:
    value = row.get(key, "")
    return float(value) if value not in {"", None} else 0.0


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


def stats(values: list[float]) -> dict[str, float]:
    return {
        "count": float(len(values)),
        "avg": mean(values) if values else 0.0,
        "p50": percentile(values, 50),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "max": max(values, default=0.0),
    }


def nonzero_values(rows: list[dict[str, str]], key: str) -> list[float]:
    return [to_float(row, key) for row in rows if to_float(row, key) > 0]


def build_freshness_samples(telemetry_rows: list[dict[str, str]], tx_rows: list[dict[str, str]]) -> list[float]:
    queue_rows = [row for row in tx_rows if row.get("source") == "queue"]
    telemetry_inference_rows = [row for row in telemetry_rows if to_float(row, "inference_us") > 0]
    count = min(len(queue_rows), len(telemetry_inference_rows))
    freshness: list[float] = []
    for index in range(count):
        tele = telemetry_inference_rows[index]
        tx = queue_rows[index]
        freshness.append(
            to_float(tele, "inference_us")
            + to_float(tele, "inference_to_enqueue_us")
            + to_float(tx, "e2e_us")
        )
    return freshness


def throughput_bps(rows: list[dict[str, str]]) -> float:
    if len(rows) < 2:
        return 0.0
    timestamps = [to_float(row, "log_time_ms") / 1000.0 for row in rows if to_float(row, "log_time_ms") > 0]
    if len(timestamps) < 2:
        return 0.0
    duration = max(timestamps) - min(timestamps)
    if duration <= 0:
        return 0.0
    total_bytes = sum(to_float(row, "len") for row in rows)
    return (total_bytes * 8.0) / duration


def summarize_telemetry(rows: list[dict[str, str]], tx_rows: list[dict[str, str]]) -> dict[str, object]:
    latest = rows[-1] if rows else {}
    rtt_samples = [to_float(row, "rtt_ms") for row in rows]
    heap_samples = [to_float(row, "heap_free") for row in rows]
    main_stack_samples = [to_float(row, "main_stack_hwm") for row in rows]
    net_tx_stack_samples = [to_float(row, "net_tx_stack_hwm") for row in rows if to_float(row, "net_tx_stack_hwm") > 0]
    inference_samples = nonzero_values(rows, "inference_us")
    inference_to_enqueue_samples = nonzero_values(rows, "inference_to_enqueue_us")
    freshness_samples = build_freshness_samples(rows, tx_rows)

    return {
        "samples": len(rows),
        "handshake_time_ms": stats([to_float(row, "handshake_time_ms") for row in rows]),
        "rtt_ms": stats(rtt_samples),
        "heap_free": stats(heap_samples),
        "main_stack_hwm": stats(main_stack_samples),
        "net_tx_stack_hwm": stats(net_tx_stack_samples),
        "inference_us": stats(inference_samples),
        "inference_to_enqueue_us": stats(inference_to_enqueue_samples),
        "freshness_us": stats(freshness_samples),
        "latest_counters": {
            "tx_queue_depth": latest.get("tx_queue_depth", "0"),
            "tx_queue_max": latest.get("tx_queue_max", "0"),
            "route_queue_full_count": latest.get("route_queue_full_count", "0"),
            "stream_send_ok": latest.get("stream_send_ok", "0"),
            "stream_send_fail": latest.get("stream_send_fail", "0"),
            "datagrams_sent": latest.get("datagrams_sent", "0"),
            "datagrams_lost": latest.get("datagrams_lost", "0"),
        },
        "plot_data": {
            "rtt_ms_samples": rtt_samples,
            "inference_us_samples": inference_samples,
            "inference_to_enqueue_us_samples": inference_to_enqueue_samples,
            "freshness_us_samples": freshness_samples,
            "heap_free_samples": heap_samples,
        },
    }


def summarize_tx(rows: list[dict[str, str]]) -> dict[str, object]:
    result: dict[str, object] = {"samples": len(rows)}
    mode_counts: dict[str, int] = {}
    mode_throughput: dict[str, float] = {}
    for mode in ("stream", "datagram"):
        scoped = [row for row in rows if row.get("mode") == mode]
        send_samples = [to_float(row, "send_us") for row in scoped]
        e2e_samples = [to_float(row, "e2e_us") for row in scoped]
        qwait_samples = [to_float(row, "qwait_us") for row in scoped]
        depth_samples = [to_float(row, "depth") for row in scoped]
        mode_counts[mode] = len(scoped)
        mode_throughput[mode] = throughput_bps(scoped)
        result[mode] = {
            "count": len(scoped),
            "qwait_us": stats(qwait_samples),
            "send_us": stats(send_samples),
            "e2e_us": stats(e2e_samples),
            "depth": stats(depth_samples),
            "failures": sum(1 for row in scoped if int(float(row.get("ret", "0") or 0)) != 0),
            "throughput_bps": mode_throughput[mode],
        }
    result["plot_data"] = {
        "stream_send_us_samples": [to_float(row, "send_us") for row in rows if row.get("mode") == "stream"],
        "datagram_send_us_samples": [to_float(row, "send_us") for row in rows if row.get("mode") == "datagram"],
        "mode_counts": mode_counts,
        "mode_throughput_bps": mode_throughput,
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize parsed AIoT QUIC monitor CSVs.")
    parser.add_argument("--telemetry", type=Path, required=True)
    parser.add_argument("--tx-events", type=Path, required=True)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()

    telemetry_rows = load_csv(args.telemetry)
    tx_rows = load_csv(args.tx_events)
    telemetry = summarize_telemetry(telemetry_rows, tx_rows)
    tx = summarize_tx(tx_rows)
    summary = {"telemetry": telemetry, "tx": tx}

    text = json.dumps(summary, indent=2)
    if args.output_json:
        args.output_json.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
