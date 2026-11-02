#!/usr/bin/env python3
"""Pair AI-result objects and exact-source JPEGs. Not a physical GT metric."""

import argparse
import json
from pathlib import Path
from analyze_remote import summarize


def coverage(events, sender_totals):
    images = {e["group"]: e for e in events if e["event"] == "object" and e.get("alias") == 1}
    results = [e for e in events if e["event"] == "object" and e.get("alias") == 2]
    required = []
    for e in results:
        r = e["result"]
        if r.get("evidence_required"):
            if r.get("evidence_group") != r["seq"] or e["group"] != r["seq"]:
                raise ValueError("evidence reference is not exact-source group")
            required.append(e)
    pairs = [e for e in required if e["group"] in images]
    # Aggregates do not identify WHICH receipts were late. Use a conservative
    # set-intersection bound instead of assigning on-time status arbitrarily.
    p = sender_totals["1"]
    a = sender_totals["2"]
    # Include receiver completions whose sender timed out before ACK.
    possible_late = (len(images) - p["ack_on_time"]) + (len(results) - a["ack_on_time"])
    lower = max(0, len(pairs) - possible_late)
    count = len(required)
    return dict(
        scope="evidence coverage of delivered AI result objects; NOT physical event DESR",
        delivered_ai_results=len(results),
        results_requiring_evidence=count,
        linked_complete_pairs=len(pairs),
        missing_evidence=count - len(pairs),
        pair_ack_on_time_lower_bound=lower,
        coverage_of_delivered_results=len(pairs) / count if count else None,
        missing_groups=[e["group"] for e in required if e["group"] not in images],
        orphan_images=sorted(set(images) - {e["group"] for e in required}),
        note="Same group reference is enforced; reviewed GT and source ledger are still needed for mission success.",
    )


def main():
    p = argparse.ArgumentParser()
    p.add_argument("run", type=Path)
    a = p.parse_args()
    summary = summarize(a.run)
    events = [
        json.loads(line)
        for line in (a.run / "events.jsonl").read_text().splitlines()
        if line.strip()
    ]
    report = coverage(events, summary["sender_totals"])
    stats = summary["sender_totals"]["1"]
    source = stats.get("source_count", 0)
    span = stats.get("source_span_us", 0)
    report["source"] = dict(
        count=source,
        failed=stats.get("source_failed"),
        measured_offer_rate_hz=(source - 1) * 1e6 / span if source > 1 and span else None,
        max_gap_ms=stats.get("source_max_gap_us", 0) / 1000,
        caveat="Read firmware input mode: replay releases do not demonstrate live sensor capture.",
    )
    report["codec"] = {k: v for k, v in stats.items() if k.startswith("codec_")}
    if stats.get("codec_calls"):
        report["codec"]["mean_wall_ms"] = stats["codec_wall_sum_us"] / stats["codec_calls"] / 1000
    (a.run / "evidence-summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
