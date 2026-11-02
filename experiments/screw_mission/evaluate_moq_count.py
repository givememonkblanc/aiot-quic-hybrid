#!/usr/bin/env python3
"""Summarize simultaneous-object counts carried on the MoQ AI track.

This is a static-scene characterization.  It gives an optimistic lower bound
on misses only: without per-screw ground-truth matching, a reported blob may be
a false positive and repeated frames are not independent mission events.
"""
import argparse
import json
import statistics
from pathlib import Path


def parse_wire(value):
    payload = bytes.fromhex(value)
    if len(payload) < 16 or payload[0] != 0xD1 or payload[1] not in (1, 2):
        raise ValueError("invalid AI result payload")
    header = 20 if payload[1] == 2 else 16
    count = payload[3]
    required = header + count * 6
    if len(payload) != required:
        raise ValueError(f"AI result length {len(payload)} != {required}")
    detections = []
    for offset in range(header, required, 6):
        fields = payload[offset:offset + 6]
        detections.append(dict(zip(
            ("class_id", "confidence", "x", "y", "width", "height"), fields)))
    return {"version": payload[1], "backend": payload[2], "count": count,
            "detections": detections}


def evaluate(run_dir, gt_count, wire_capacity):
    objects = []
    for line_no, line in enumerate((run_dir / "events.jsonl").read_text().splitlines(), 1):
        event = json.loads(line)
        if event.get("event") != "object" or event.get("alias") != 2:
            continue
        result = event.get("result") or {}
        wire = parse_wire(result["result_hex"])
        declared = int(result["detections"])
        if declared != wire["count"]:
            raise ValueError(f"line {line_no}: JSON count {declared} != wire {wire['count']}")
        objects.append({"group": event["group"], "seq": result["seq"],
                        "count": wire["count"], "detections": wire["detections"]})
    if not objects:
        raise ValueError("no MoQ AI objects found")
    counts = [item["count"] for item in objects]
    best = max(counts)
    median = statistics.median(counts)
    return {
        "run": run_dir.name,
        "ground_truth_simultaneous_objects": gt_count,
        "ai_objects_received": len(objects),
        "wire_capacity": wire_capacity,
        "wire_capacity_hit_frames": sum(count >= wire_capacity for count in counts),
        "reported_count": {
            "min": min(counts), "median": median, "max": best,
            "per_frame": counts,
        },
        "optimistic_static_count_bound": {
            "minimum_missed_in_best_frame": max(0, gt_count - best),
            "missed_at_median_count": max(0, gt_count - median),
            "best_frame_recall_ceiling": min(1.0, best / gt_count),
        },
        "interpretation": (
            "Optimistic bound only: every reported blob is treated as a true screw. "
            "False positives can only increase the miss count. Repeated static frames "
            "do not measure unique turntable passage success."
        ),
        "objects": objects,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--gt-count", type=int, required=True)
    parser.add_argument("--wire-capacity", type=int, default=24)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = evaluate(args.run_dir, args.gt_count, args.wire_capacity)
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")


if __name__ == "__main__":
    main()
