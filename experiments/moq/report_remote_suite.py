#!/usr/bin/env python3
"""Render only completed trial records from the remote suite journal."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics
import time


def render(directory):
    state = json.loads((directory / "manifest.json").read_text())
    groups = defaultdict(list)
    for row in state["results"]:
        groups[(row["policy"], row["network"])].append(row["summary"])
    text = ["# Remote comparison progress", "",
            f"Status: {state['status']}; validated {len(state['results'])}/{len(state['cells'])}.",
            f"Active/last trial: {state.get('active')}", "",
            "Fixed replay, host network impairment; no physical screw mission claim.",
            "Cells show mean [min, max] across runs. No statistical superiority is inferred.", "",
            "| Policy | Network | n | Preview objects | AI objects | JPEG kbps | AI source-to-ACK p95 seconds |",
            "|---|---|---:|---|---|---|---|"]
    def fmt(values):
        values = [x for x in values if x is not None]
        if not values:
            return "unavailable"
        return f"{statistics.mean(values):.3f} [{min(values):.3f}, {max(values):.3f}]"
    for (policy, network), runs in sorted(groups.items()):
        text.append(f"| {policy} | {network} | {len(runs)} | " + " | ".join([
            fmt([r["preview"]["objects"] for r in runs]),
            fmt([r["ai"]["objects"] for r in runs]),
            fmt([r["preview"]["payload_goodput_bps"] / 1000 for r in runs]),
            fmt([r["ai"]["source_to_ack_p95_s"] for r in runs]),
        ]) + " |")
    if state.get("reason"):
        text.extend(["", "Stopped: " + state["reason"]])
    text.extend(["", "Raw per-run summaries retain representation sizes, stream resets, sender totals",
                 "and deadline accounting. AI receipts are not correct detections; preview counts",
                 "alone do not compare quality when representations differ."])
    tmp = directory / "RESULTS.tmp"
    tmp.write_text("\n".join(text) + "\n")
    tmp.replace(directory / "RESULTS.md")
    return state["status"]


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--watch", action="store_true")
    args = p.parse_args()
    while True:
        status = render(args.directory)
        if not args.watch or status in ("complete", "stopped"):
            break
        time.sleep(10)
