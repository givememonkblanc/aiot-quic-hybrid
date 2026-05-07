#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path


TINYML_FIELDS = [
    "timestamp_us",
    "type",
    "payload_len",
    "rtt_ms",
    "loss_percent",
    "tx_queue_depth",
    "datagram_budget",
    "handshake_time_ms",
    "stream_window_available",
    "quic_ready",
    "cpu_usage",
    "heap_free",
    "heap_caps",
    "main_stack_hwm",
    "net_tx_stack_hwm",
    "tx_queue_max",
    "route_queue_full_count",
    "stream_send_ok",
    "stream_send_fail",
    "datagrams_sent",
    "datagrams_lost",
    "inference_us",
    "inference_to_enqueue_us",
    "decision_mode",
    "pacing_level",
]

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
KV_RE = re.compile(r"([A-Za-z0-9_]+)=([^,\s]+)")
LOG_TIME_RE = re.compile(r"[A-Z] \((\d+)\)")
TX_RE = re.compile(
    r"\[(STREAM|DATAGRAM)\]\s+src=([A-Za-z0-9_]+)\s+len=(\d+)\s+qwait_us=(\d+)\s+send_us=(\d+)\s+e2e_us=(\d+)\s+depth=(\d+)\s+ret=(-?\d+)"
)


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def parse_tinyml(line: str) -> dict[str, str] | None:
    marker = "[TINYML-DATA],"
    if marker not in line:
        return None
    payload = line.split(marker, 1)[1].strip()
    extra = {}
    if ",ai=" in payload:
        payload, suffix = payload.split(",ai=", 1)
        extra["ai"] = suffix
        for key, value in KV_RE.findall("ai=" + suffix):
            extra[key] = value
    parts = [part.strip() for part in payload.split(",")]
    if len(parts) < len(TINYML_FIELDS):
        return None
    row = {name: parts[index] for index, name in enumerate(TINYML_FIELDS)}
    row.update(extra)
    return row


def parse_tx(line: str) -> dict[str, str] | None:
    match = TX_RE.search(line)
    if not match:
        return None
    time_match = LOG_TIME_RE.search(line)
    mode, source, length, qwait, send_us, e2e, depth, ret = match.groups()
    return {
        "log_time_ms": time_match.group(1) if time_match else "",
        "mode": mode.lower(),
        "source": source,
        "len": length,
        "qwait_us": qwait,
        "send_us": send_us,
        "e2e_us": e2e,
        "depth": depth,
        "ret": ret,
    }


def parse_summary(line: str) -> dict[str, str] | None:
    marker = "[APP-SUMMARY]"
    if marker not in line:
        return None
    payload = line.split(marker, 1)[1]
    row = {key: value for key, value in KV_RE.findall(payload)}
    return row or None


def write_csv(path: Path, rows: list[dict[str, str]], fieldnames: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in fieldnames})


def main() -> int:
    parser = argparse.ArgumentParser(description="Parse ESP-IDF monitor logs into CSV files.")
    parser.add_argument("log", type=Path)
    parser.add_argument("--out-dir", type=Path, default=Path("."))
    args = parser.parse_args()

    telemetry_rows: list[dict[str, str]] = []
    tx_rows: list[dict[str, str]] = []
    summary_rows: list[dict[str, str]] = []

    with args.log.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line = strip_ansi(raw_line.rstrip("\n"))
            tinyml = parse_tinyml(line)
            if tinyml is not None:
                telemetry_rows.append(tinyml)
                continue
            tx = parse_tx(line)
            if tx is not None:
                tx_rows.append(tx)
                continue
            summary = parse_summary(line)
            if summary is not None:
                summary_rows.append(summary)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.out_dir / "telemetry.csv", telemetry_rows, TINYML_FIELDS + ["ai", "backend", "det"])
    write_csv(args.out_dir / "tx_events.csv", tx_rows, ["log_time_ms", "mode", "source", "len", "qwait_us", "send_us", "e2e_us", "depth", "ret"])

    summary_keys = sorted({key for row in summary_rows for key in row})
    if summary_keys:
        write_csv(args.out_dir / "app_summary.csv", summary_rows, summary_keys)

    print(f"telemetry_rows={len(telemetry_rows)}")
    print(f"tx_rows={len(tx_rows)}")
    print(f"summary_rows={len(summary_rows)}")
    print(f"output_dir={args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
