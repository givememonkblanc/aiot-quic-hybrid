#!/usr/bin/env python3
"""Validate frozen-input ladder runs and emit per-run tables/figures.
Run-level uncertainty only; transport outcomes are not mission success.
"""

import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import statistics
from analyze_remote import summarize

BASE = Path(__file__).resolve().parent
POLICIES = ["high", "high_timeout", "low", "medium", "adaptive"]


def read_events(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def run_row(root, cell, manifest):
    directory = root / cell["run"]
    trial = json.loads((directory / "manifest.json").read_text())
    if trial["status"] != "complete" or not trial.get("restore_verified"):
        raise ValueError("incomplete trial or restore")
    if trial["test_release"]["image_sha256"] != manifest["firmware"][cell["policy"]]["sha256"]:
        raise ValueError("firmware differs from frozen suite")
    summary = summarize(directory)
    events = read_events(directory / "events.jsonl")
    allowed = {r["sha256"]: r for r in manifest["ladder"]["representations"]}
    counts = Counter()
    images = [e for e in events if e["event"] == "object" and e.get("alias") == 1]
    for e in images:
        rep = allowed.get(e["sha256"])
        if rep is None:
            raise ValueError("received payload outside frozen ladder")
        raw = (directory / f"frame-{e['group']:06d}.jpg").read_bytes()
        if hashlib.sha256(raw).hexdigest() != e["sha256"]:
            raise ValueError("archived image hash mismatch")
        if (e["width"], e["height"], e["bytes"]) != (rep["width"], rep["height"], rep["bytes"]):
            raise ValueError("representation shape/length mismatch")
        counts[rep["name"]] += 1
    sender = summary["sender_totals"]
    if set(sender) != {"1", "2"}:
        raise ValueError("missing full sender accounting")
    detection_counts = []
    signatures = set()
    for e in events:
        if e["event"] != "object" or e.get("alias") != 2:
            continue
        result = e["result"]
        wire = bytes.fromhex(result["result_hex"])
        if (
            wire[:2] != b"\xd1\x02"
            or len(wire) != 20 + 6 * wire[3]
            or wire[3] != result["detections"]
        ):
            raise ValueError("AI detection encoding mismatch")
        detection_counts.append(wire[3])
        signatures.add(hashlib.sha256(wire[20:]).hexdigest())
    start = max(e["t"] for e in events if e["event"] == "subscribed")
    end = next(e["t"] for e in events if e["event"] == "finished")
    stamps = [start] + [e["t"] for e in images] + [end]
    gaps = [b - a for a, b in zip(stamps, stamps[1:])]
    # Include zero-delivery runs, initial gap and drain tail. Also separately
    # report gaps over the nominal 60-s offer phase, not inferred capture rate.
    active_end = start + 60
    active = [start] + [e["t"] for e in images if e["t"] <= active_end] + [active_end]
    active_gaps = [b - a for a, b in zip(active, active[1:])]
    rows = dict(
        run=cell["run"],
        policy=cell["policy"],
        network=cell["network"],
        repeat=cell["repeat"],
        preview_complete=len(images),
        preview_high=counts["high"],
        preview_medium=counts["medium"],
        preview_low=counts["low"],
        preview_ack_on_time=sender["1"]["ack_on_time"],
        preview_resets=sender["1"]["timeout_resets"],
        preview_feasibility_rejects=sender["1"]["feasibility_rejects"],
        preview_probes=sender["1"]["probes"],
        preview_goodput_kbps=summary["preview"]["payload_goodput_bps"] / 1000,
        preview_max_gap_full_s=max(gaps),
        preview_max_gap_offer_window_s=max(active_gaps),
        preview_mean_ack_age_s=sender["1"]["ack_age_sum_us"] / 1e6 / sender["1"]["completed"]
        if sender["1"]["completed"]
        else None,
        ai_complete=summary["ai"]["objects"],
        ai_ack_on_time=sender["2"]["ack_on_time"],
        ai_source_to_ack_p95_s=summary["ai"]["source_to_ack_p95_s"],
        ai_source_to_ack_max_full_s=sender["2"]["ack_age_max_us"] / 1e6,
        ai_capture_to_observed_p95_s=summary["ai"]["capture_to_observed_p95_s"],
        ai_unique_detection_outputs=len(signatures),
        ai_detection_count_min=min(detection_counts, default=None),
        ai_detection_count_max=max(detection_counts, default=None),
        ai_detection_signature=next(iter(signatures)) if len(signatures) == 1 else None,
        min_free_internal_bytes=sender["1"]["min_internal"],
        min_free_psram_bytes=sender["1"]["min_psram"],
    )
    epochs = []
    for begin, finish in [(0, 20), (20, 40), (40, 60)]:
        epoch_images = [e for e in images if begin <= e["t"] - start < finish]
        c = Counter(allowed[e["sha256"]]["name"] for e in epoch_images)
        epochs.append(
            dict(start_s=begin, end_s=finish, received=len(epoch_images), representations=dict(c))
        )
    return rows, dict(run=cell["run"], epochs=epochs, rate_changes=trial.get("rate_changes", []))


def aggregate(rows):
    groups = defaultdict(list)
    for r in rows:
        groups[r["policy"], r["network"]].append(r)
    result = []
    metrics = [
        "preview_complete",
        "preview_ack_on_time",
        "preview_max_gap_offer_window_s",
        "ai_complete",
        "ai_source_to_ack_p95_s",
        "min_free_internal_bytes",
        "min_free_psram_bytes",
    ]
    for (policy, network), runs in groups.items():
        item = dict(policy=policy, network=network, n=len(runs), metrics={})
        for key in metrics:
            values = [r[key] for r in runs if r[key] is not None]
            item["metrics"][key] = dict(
                mean=statistics.mean(values) if values else None,
                minimum=min(values) if values else None,
                maximum=max(values) if values else None,
                n=len(values),
                confidence_interval=None,
            )
        result.append(item)
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("suite", type=Path)
    p.add_argument("--plot", action="store_true")
    a = p.parse_args()
    manifest = json.loads((a.suite / "manifest.json").read_text())
    rows = []
    epochs = []
    invalid = []
    for cell in manifest["results"]:
        if cell["status"] == "running":
            continue
        try:
            row, epoch = run_row(BASE / "results", cell, manifest)
            rows.append(row)
            epochs.append(epoch)
        except (KeyError, ValueError, OSError) as error:
            invalid.append(dict(run=cell["run"], reason=str(error)))
    report = dict(
        scope="fixed-input replay transport characterization; no event GT",
        suite_status=manifest["status"],
        network_profiles=manifest.get("network_profiles", {}),
        network_scope=manifest.get("network_scope", "See trial manifests"),
        planned_runs=len(manifest["cells"]),
        valid_runs=len(rows),
        invalid_runs=invalid,
        rows=rows,
        epochs=epochs,
        aggregates=aggregate(rows),
        limitations=[
            "offline representations",
            "no receiver/source clock mapping",
            "ACK on-time is sufficient, not necessary, for on-time receive",
            "no confidence intervals or superiority claims from screening n=1",
        ],
    )
    (a.suite / "comparison.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    if rows:
        with (a.suite / "comparison.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    lines = [
        "# Frozen-input representation screening",
        "",
        f"Valid {len(rows)} / planned {len(manifest['cells'])} runs. Scope: **transport**, not screw mission success.",
        "",
        "| Network | Policy | H/M/L complete | Preview ACK≤2s | Resets | AI received / ACK≤10s | AI ACK p95 (s) | Max preview gap, offer window (s) |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    policy_order = list(manifest["firmware"])
    for r in sorted(
        rows, key=lambda r: (r["network"], policy_order.index(r["policy"]), r["repeat"])
    ):
        ack = r["ai_source_to_ack_p95_s"]
        lines.append(
            f"| {r['network']} | {r['policy']} | {r['preview_high']}/{r['preview_medium']}/{r['preview_low']} | {r['preview_ack_on_time']} | {r['preview_resets']} | {r['ai_complete']}/{r['ai_ack_on_time']} | {ack:.3f} | {r['preview_max_gap_offer_window_s']:.3f} |"
            if ack is not None
            else f"| {r['network']} | {r['policy']} | incomplete AI sampling |"
        )
    lines.extend(
        [
            "",
            "H=1600×1200, M=800×600, L=400×300. Each input/received JPEG hash validated.",
            "A received preview need not be the source frame of an AI event. This table is not evidence-linked mission success.",
            "One run per cell is screening. Receiver gaps include initial/final gaps in the nominal offer window.",
            "AI ACK p95 uses deduplicated piggyback receipts; the last ACK can be missing. Aggregate on-time totals cover all admitted streams.",
            "Frames within a run are not independent repetitions.",
        ]
    )
    (a.suite / "RESULTS.md").write_text("\n".join(lines) + "\n")
    if a.plot and rows:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np

        networks = sorted({r["network"] for r in rows})
        fig, axes = plt.subplots(len(networks), 2, figsize=(11, 4 * len(networks)), squeeze=False)
        for i, network in enumerate(networks):
            selected = [r for r in rows if r["network"] == network]
            selected.sort(key=lambda r: (policy_order.index(r["policy"]), r["repeat"]))
            x = np.arange(len(selected))
            bottom = np.zeros(len(selected))
            for key, label in [
                ("preview_high", "1600×1200"),
                ("preview_medium", "800×600"),
                ("preview_low", "400×300"),
            ]:
                vals = np.array([r[key] for r in selected])
                axes[i, 0].bar(x, vals, bottom=bottom, label=label)
                bottom += vals
            axes[i, 0].set_ylabel("Complete preview objects")
            axes[i, 0].set_title(f"{network}: representation delivery")
            axes[i, 0].legend(fontsize=8)
            axes[i, 1].scatter(
                x, [r["ai_source_to_ack_p95_s"] for r in selected], label="AI source→ACK p95"
            )
            axes[i, 1].set_ylabel("Seconds")
            axes[i, 1].set_title(f"{network}: AI latency (device clock)")
            for ax in axes[i]:
                ax.set_xticks(x, [r["policy"] for r in selected], rotation=25, ha="right")
                ax.grid(axis="y", alpha=0.2)
        fig.suptitle(
            "ESP32-S3 frozen-input screening — per-run observations, no significance claim"
        )
        fig.tight_layout()
        fig.savefig(a.suite / "screening.pdf")
        fig.savefig(a.suite / "screening.png", dpi=180)
        plt.close(fig)
    print(json.dumps(dict(valid_runs=len(rows), invalid=invalid, suite_status=manifest["status"])))
    if invalid:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
