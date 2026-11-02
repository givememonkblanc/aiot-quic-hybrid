#!/usr/bin/env python3
"""Summarize a completed remote-OTA dual-track MoQ receiver run."""

import argparse
import json
import statistics
from pathlib import Path


def percentile(values, q):
    values = sorted(values)
    if not values:
        return None
    position = (len(values) - 1) * q
    lower = int(position)
    upper = min(lower + 1, len(values) - 1)
    fraction = position - lower
    return values[lower] * (1 - fraction) + values[upper] * fraction


def intervals(objects):
    return [b["t"] - a["t"] for a, b in zip(objects, objects[1:])]


def summarize(run):
    events = [json.loads(line) for line in (run / "events.jsonl").read_text().splitlines() if line.strip()]
    errors = [event for event in events if event["event"] == "error"]
    finished = [event for event in events if event["event"] == "finished"]
    subscribed = [event for event in events if event["event"] == "subscribed"]
    preview = [event for event in events if event["event"] == "object" and event.get("alias") == 1]
    ai = [event for event in events if event["event"] == "object" and event.get("alias") == 2]
    resets = [
        event for event in events if event["event"] == "reset" and event.get("alias") in (1, 2)
    ]
    unknown_resets = [
        event for event in events if event["event"] == "reset" and event.get("alias") is None
    ]
    if errors or len(finished) != 1 or len(subscribed) != 2:
        raise ValueError(
            f"incomplete run: errors={len(errors)} finished={len(finished)} "
            f"subscriptions={len(subscribed)}"
        )
    start = min(event["t"] for event in subscribed)
    end = finished[0]["t"]
    window = end - start
    preview_intervals = intervals(preview)
    ai_intervals = intervals(ai)
    inference_ages = []
    detections = []
    source_to_ack = []
    ack_groups = {}
    for event in ai:
        result = event["result"]
        inference_ages.append(result["observed_us"] / 1e6 - result["capture_ms"] / 1e3)
        detections.append(result["detections"])
        if result.get("prev_ack_valid"):
            group = result["prev_ack_group"]
            value = result["prev_source_to_ack_us"] / 1e6
            if group in ack_groups and ack_groups[group] != value:
                raise ValueError("conflicting repeated ACK telemetry")
            ack_groups[group] = value
    source_to_ack = list(ack_groups.values())
    sender = {}
    for event in events:
        if event["event"] == "publish_done" and event.get("reason", "").startswith("{"):
            data = json.loads(event["reason"])
            if data.get("schema") == 1:
                alias = event["alias"]
                received = len(preview) if alias == 1 else len(ai)
                reset_count = sum(e["alias"] == alias for e in resets)
                # A receiver can consume FIN before the sender times out waiting
                # for its ACK. RESET_STREAM then need not reach the application.
                # Reconcile stream conservation, not equal terminal labels.
                if (
                    data["completed"] + data["timeout_resets"] != event["streams"]
                    or event["streams"] != received + reset_count
                    or not 0 <= data["completed"] <= received
                    or data["timeout_resets"] < reset_count
                    or not 0 <= data["ack_on_time"] <= data["completed"]
                ):
                    raise ValueError("sender/receiver terminal accounting mismatch")
                data["receiver_completed"] = received
                data["receiver_resets"] = reset_count
                data["receiver_complete_sender_unconfirmed"] = received - data["completed"]
                sender[str(alias)] = data
    if unknown_resets:
        raise ValueError("unattributed stream resets invalidate per-track accounting")
    groups = [event["group"] for event in preview]
    summary = {
        "run": run.name,
        "status": "complete",
        "alpn": next(event["alpn"] for event in events if event["event"] == "handshake"),
        "receiver_window_s": window,
        "metric_semantics": {
            "capture_to_observed": "device capture timestamp to application observation of AI result; NOT receiver latency or isolated inference time",
            "source_to_ack": "device-clock QUIC stream completion ACK; conservative delivery bound, last receipt may be absent from piggyback samples",
            "ack_on_time": "all stream-close receipts from sender summary, not piggyback samples; sufficient condition for receiver arrival before deadline",
            "scope": "transport service; no ground-truth mission success inferred",
        },
        "sender_totals": sender,
        "preview": {
            "objects": len(preview),
            "timeout_resets": sum(event["alias"] == 1 for event in resets),
            "bytes": sum(event["bytes"] for event in preview),
            "payload_goodput_bps": sum(event["bytes"] for event in preview) * 8 / window,
            "interval_p50_s": percentile(preview_intervals, 0.5),
            "interval_p95_s": percentile(preview_intervals, 0.95),
            "interval_max_s": max(preview_intervals, default=None),
            "first_group": groups[0] if groups else None,
            "last_group": groups[-1] if groups else None,
            "group_step_p50": percentile([b - a for a, b in zip(groups, groups[1:])], 0.5),
            "all_jpeg_dimensions": sorted({(event["width"], event["height"]) for event in preview}),
        },
        "ai": {
            "objects": len(ai),
            "timeout_resets": sum(event["alias"] == 2 for event in resets),
            "bytes": sum(event["bytes"] for event in ai),
            "interval_p50_s": percentile(ai_intervals, 0.5),
            "interval_p95_s": percentile(ai_intervals, 0.95),
            "capture_to_observed_p50_s": percentile(inference_ages, 0.5),
            "capture_to_observed_p95_s": percentile(inference_ages, 0.95),
            "capture_to_observed_min_s": min(inference_ages, default=None),
            "capture_to_observed_max_s": max(inference_ages, default=None),
            "detections_positive": sum(value > 0 for value in detections),
            "source_to_ack_samples": len(source_to_ack),
            "source_to_ack_p50_s": percentile(source_to_ack, 0.5),
            "source_to_ack_p95_s": percentile(source_to_ack, 0.95),
            "source_to_ack_max_s": max(source_to_ack, default=None),
        },
        "integrity": {
            "track_finished": {
                str(event["alias"]): {"objects": event["objects"], "resets": event.get("resets", 0)}
                for event in events
                if event["event"] == "track_finished"
            },
            "unknown_resets": unknown_resets,
            "closed_cleanly": any(
                event["event"] == "closed" and event["code"] == 0 for event in events
            ),
            "errors": errors,
        },
    }
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    summary = summarize(args.run)
    output = args.output or args.run / "remote-summary.json"
    output.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
