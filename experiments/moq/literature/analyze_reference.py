#!/usr/bin/env python3
"""Validate reference transport trials, with offered media in every denominator."""

import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import statistics

DEADLINES = [0.1, 0.5, 1.0]


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    at = (len(values) - 1) * fraction
    lo, hi = math.floor(at), math.ceil(at)
    return values[lo] + (values[hi] - values[lo]) * (at - lo)


def timeout_row(data, cell):
    total = data["offered_frames"]
    count = data["descriptions_per_frame"]
    produced = data["produced_s"]
    assert len(produced) == total and all(a <= b for a, b in zip(produced, produced[1:]))
    descriptions = {}
    duplicates = 0
    ages = []
    for event in data["events"]:
        if event["event"] != "description":
            continue
        frame, tile = event["frame"], event["tile"]
        assert 0 <= frame < total and 0 <= tile < count and event["payload_valid"]
        assert event["group"] == frame // data["group_frames"]
        age = event["received_s"] - produced[frame]
        assert age >= 0
        key = (frame, tile)
        if key in descriptions:
            duplicates += 1
        descriptions[key] = min(age, descriptions.get(key, math.inf))
        ages.append(age)
    coverage = [
        sum((frame, tile) in descriptions for tile in range(count)) for frame in range(total)
    ]
    row = dict(
        **cell,
        offered_frames=total,
        offered_descriptions=total * count,
        received_descriptions=len(descriptions),
        duplicate_descriptions=duplicates,
        complete_frames=sum(n == count for n in coverage),
        zero_description_frames=sum(n == 0 for n in coverage),
        spatial_coverage_fraction=sum(coverage) / (total * count),
        received_age_p95_s=percentile(ages, 0.95),
        group_read_errors=sum(e["event"] == "group_error" for e in data["events"]),
        udp_payload_forwarded_bytes=data["bottleneck"]["forwarded_bytes"],
        udp_queue_drops=data["bottleneck"]["queue_drops"],
    )
    for deadline in DEADLINES:
        name = str(int(deadline * 1000))
        row["complete_frames_within_" + name + "ms"] = sum(
            all(descriptions.get((frame, tile), math.inf) <= deadline for tile in range(count))
            for frame in range(total)
        )
        row["coverage_within_" + name + "ms"] = sum(
            age <= deadline for age in descriptions.values()
        ) / (total * count)
    return row


def switching_row(data, cell):
    total = data["offered_frames"]
    produced = data["produced_s"]
    assert len(produced) == total and all(a <= b for a, b in zip(produced, produced[1:]))
    frames = defaultdict(list)
    objects = []
    ages = []
    for event in data["objects"]:
        if not event["bytes"]:
            continue
        frame = event["group"] * data["group_frames"] + event["object"]
        assert 0 <= frame < total and event["payload_valid"]
        assert event["alias"] in (data["old_alias"], data["new_alias"])
        assert event["t"] >= produced[frame]
        frames[frame].append(event)
        objects.append(event)
        ages.append(event["t"] - produced[frame])
    new = [e for e in objects if e["alias"] == data["new_alias"]]
    duplicate_bytes = sum(
        sum(e["bytes"] for e in sorted(events, key=lambda e: e["t"])[1:])
        for events in frames.values()
    )
    complete_bytes = sum(e["bytes"] for e in objects)
    row = dict(
        **cell,
        offered_frames=total,
        received_objects=len(objects),
        unique_frames=len(frames),
        missing_frames=total - len(frames),
        duplicate_objects=len(objects) - len(frames),
        duplicate_complete_payload_bytes=duplicate_bytes,
        duplicate_complete_payload_fraction=duplicate_bytes / complete_bytes
        if complete_bytes
        else None,
        switch_first_new_s=min((e["t"] for e in new), default=math.inf) - data["switch_request_s"]
        if new
        else None,
        received_age_p95_s=percentile(ages, 0.95),
        complete_payload_bytes=complete_bytes,
    )
    for deadline in DEADLINES:
        row["frames_within_" + str(int(deadline * 1000)) + "ms"] = sum(
            min(e["t"] for e in events) - produced[frame] <= deadline
            for frame, events in frames.items()
        )
    return row


def main():
    p = argparse.ArgumentParser()
    p.add_argument("suite", type=Path)
    p.add_argument("--kind", choices=["timeout", "switch"], required=True)
    a = p.parse_args()
    manifest = json.loads((a.suite / "manifest.json").read_text())
    rows = []
    for cell in manifest["results"]:
        if cell["status"] != "complete":
            raise ValueError("failed trial is not a completed result")
        data = json.loads((a.suite / cell["file"]).read_text())
        rows.append((timeout_row if a.kind == "timeout" else switching_row)(data, cell))
    keys = (
        ["timeout_ms", "group_frames", "rate_kbps"]
        if a.kind == "timeout"
        else ["mode", "delay_ms", "rate_kbps"]
    )
    groups = defaultdict(list)
    for row in rows:
        groups[tuple(row[key] for key in keys)].append(row)
    aggregates = []
    for key, items in sorted(groups.items()):
        metrics = {}
        for name in rows[0]:
            if name in keys or name in ("repeat", "file", "status"):
                continue
            values = [row[name] for row in items if isinstance(row.get(name), (int, float))]
            metrics[name] = dict(
                n=len(values),
                mean=statistics.mean(values) if values else None,
                min=min(values) if values else None,
                max=max(values) if values else None,
            )
        aggregates.append(dict(**dict(zip(keys, key)), n=len(items), metrics=metrics))
    report = dict(
        kind=a.kind,
        suite_status=manifest["status"],
        valid_runs=len(rows),
        planned_runs=len(manifest["cells"]),
        scope=manifest["scope"],
        rows=rows,
        aggregates=aggregates,
        limitations=[
            "Run-level ranges, no frame-level significance testing.",
            "JPEG workload adaptation; not original point-cloud/encoded-video reproduction.",
            "No screw detection ground truth or mission success claim.",
            "Switch duplicate-complete-payload fraction is NOT paper relay-egress AETR.",
        ],
    )
    (a.suite / "comparison.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    with (a.suite / "comparison.csv").open("w") as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]) if rows else [])
        writer.writeheader()
        writer.writerows(rows)
    lines = [
        "# Author implementation on an adapted JPEG workload",
        "",
        f"Validated {len(rows)} / {len(manifest['cells'])} planned runs. Status: {manifest['status']}.",
        "",
        "| Condition | n | "
        + (
            "Complete frames / 60 | Zero-description frames | Coverage ≤500ms"
            if a.kind == "timeout"
            else "Unique frames / 100 | Duplicate objects | First new track (s)"
        )
        + " |",
        "|---|---:|---:|---:|---:|",
    ]
    metrics = (
        ["complete_frames", "zero_description_frames", "coverage_within_500ms"]
        if a.kind == "timeout"
        else ["unique_frames", "duplicate_objects", "switch_first_new_s"]
    )
    for item in aggregates:
        cells = []
        for metric in metrics:
            m = item["metrics"][metric]
            cells.append(
                "none" if m["mean"] is None else f"{m['mean']:.3f} [{m['min']:.3f}, {m['max']:.3f}]"
            )
        lines.append(
            "| "
            + " / ".join(str(item[k]) for k in keys)
            + f" | {item['n']} | "
            + " | ".join(cells)
            + " |"
        )
    lines += ["", "Values are run means [minimum, maximum].", *report["limitations"]]
    (a.suite / "RESULTS.md").write_text("\n".join(lines) + "\n")
    print(
        json.dumps(
            dict(
                valid_runs=len(rows),
                planned_runs=len(manifest["cells"]),
                suite_status=manifest["status"],
            )
        )
    )


if __name__ == "__main__":
    main()
