#!/usr/bin/env python3
"""Aggregate QUIC frame-level signaling overhead from picoquic qlog files.

Quantifies the wire cost of stream-based vs datagram-based delivery the same
way Saif & Matrawy did for DoQ (memory/CPU/signaling per exchange): counts and
payload bytes per frame type, per direction, plus total wire bytes, so the
stream bookkeeping overhead (MAX_STREAMS, MAX_STREAM_DATA, ...) becomes
directly comparable across transport modes.

The server writes qlogs when started with `picoquicdemo ... -q qlog_dir`
(see host_receiver/run_server.sh). Direction is from the server's viewpoint:
`packet_received` carries the ESP32's uplink traffic.

Usage:
    python3 analyze_qlog_overhead.py host_receiver/qlog_dir/*.qlog --json out.json
"""
from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

# Frame payload bytes are only explicit for these qlog frame types.
PAYLOAD_FRAME_TYPES = {"stream", "datagram", "crypto"}
FLOW_CONTROL_FRAME_TYPES = {
    "max_streams", "max_stream_data", "max_data",
    "streams_blocked", "stream_data_blocked", "data_blocked",
    "reset_stream", "stop_sending",
}


def load_qlog(path: Path) -> dict | None:
    text = path.read_text(encoding="utf-8", errors="replace")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass
    # picoquic finalizes the JSON on graceful close; a run cut short (device
    # reset, Ctrl-C) leaves it truncated. Trim to the last complete event and
    # close the brackets.
    last = text.rfind("],")
    while last > 0:
        candidate = text[: last + 1] + "]}]}"
        try:
            return json.loads(candidate)
        except json.JSONDecodeError:
            last = text.rfind("],", 0, last)
    return None


def iter_events(doc: dict):
    for trace in doc.get("traces", []):
        for event in trace.get("events", []):
            if not isinstance(event, list) or not event:
                continue
            data = event[-1]
            name = next((item for item in event[1:-1] if isinstance(item, str)), "")
            names = [item for item in event if isinstance(item, str)]
            if isinstance(data, dict):
                yield names, data


def summarize_file(path: Path) -> dict[str, dict] | None:
    doc = load_qlog(path)
    if doc is None:
        return None

    stats: dict[str, dict] = {
        direction: {
            "packets": 0,
            "wire_bytes": 0,
            "frame_counts": defaultdict(int),
            "frame_payload_bytes": defaultdict(int),
        }
        for direction in ("sent", "received")
    }

    for names, data in iter_events(doc):
        if "packet_sent" in names:
            direction = "sent"
        elif "packet_received" in names:
            direction = "received"
        else:
            continue
        entry = stats[direction]
        entry["packets"] += 1
        header = data.get("header", {})
        entry["wire_bytes"] += int(header.get("packet_size", 0) or 0)
        for frame in data.get("frames", []) or []:
            if not isinstance(frame, dict):
                continue
            ftype = str(frame.get("frame_type", "unknown"))
            entry["frame_counts"][ftype] += 1
            if ftype in PAYLOAD_FRAME_TYPES:
                entry["frame_payload_bytes"][ftype] += int(frame.get("length", 0) or 0)
    return stats


def merge(total: dict[str, dict], part: dict[str, dict]) -> None:
    for direction, entry in part.items():
        acc = total[direction]
        acc["packets"] += entry["packets"]
        acc["wire_bytes"] += entry["wire_bytes"]
        for key, value in entry["frame_counts"].items():
            acc["frame_counts"][key] += value
        for key, value in entry["frame_payload_bytes"].items():
            acc["frame_payload_bytes"][key] += value


def finalize(entry: dict) -> dict:
    payload = sum(entry["frame_payload_bytes"].values())
    stream_payload = entry["frame_payload_bytes"].get("stream", 0)
    datagram_payload = entry["frame_payload_bytes"].get("datagram", 0)
    flow_control_frames = sum(
        count for ftype, count in entry["frame_counts"].items()
        if ftype in FLOW_CONTROL_FRAME_TYPES
    )
    return {
        "packets": entry["packets"],
        "wire_bytes": entry["wire_bytes"],
        "app_payload_bytes": payload,
        "stream_payload_bytes": stream_payload,
        "datagram_payload_bytes": datagram_payload,
        "overhead_bytes": max(entry["wire_bytes"] - payload, 0),
        "overhead_ratio": (
            (entry["wire_bytes"] - payload) / entry["wire_bytes"]
            if entry["wire_bytes"] > 0 else 0.0
        ),
        "flow_control_frames": flow_control_frames,
        "frame_counts": dict(sorted(entry["frame_counts"].items())),
        "frame_payload_bytes": dict(sorted(entry["frame_payload_bytes"].items())),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize frame-level overhead from picoquic qlog files.")
    parser.add_argument("qlogs", nargs="+", type=Path, help=".qlog files or directories containing them")
    parser.add_argument("--json", type=Path, help="Optional JSON output path")
    args = parser.parse_args()

    files: list[Path] = []
    for spec in args.qlogs:
        if spec.is_dir():
            files.extend(sorted(spec.glob("*.qlog")))
        else:
            files.append(spec)
    if not files:
        parser.error("no qlog files found")

    total: dict[str, dict] = {
        direction: {
            "packets": 0,
            "wire_bytes": 0,
            "frame_counts": defaultdict(int),
            "frame_payload_bytes": defaultdict(int),
        }
        for direction in ("sent", "received")
    }

    parsed = 0
    for path in files:
        part = summarize_file(path)
        if part is None:
            print(f"warning: could not parse {path}")
            continue
        parsed += 1
        merge(total, part)

    result = {
        "files_parsed": parsed,
        "files_total": len(files),
        # server perspective: "received" = ESP32 uplink, "sent" = downlink
        "uplink_from_device": finalize(total["received"]),
        "downlink_to_device": finalize(total["sent"]),
    }

    print(json.dumps(result, indent=2))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
