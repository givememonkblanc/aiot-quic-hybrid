#!/usr/bin/env python3
"""Mission accounting for passage events: what the device produced against what arrived.

The count is the number of passage events. This compares the source ledger of the device
with the receiver log, so it measures delivery. Whether the device detected every physical
passage is a separate question that needs labelled ground truth (see
experiments/screw_mission/mission_metrics.py)."""

import argparse
import json
import statistics
from pathlib import Path


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, round(q * (len(ordered) - 1))))]


def evaluate(ledger, events, outages=()):
    """`ledger`: results produced on the device, each with group, capture_t, passages_new,
    passages_total. `events`: receiver records. Times share one clock."""
    produced = {r["group"]: r for r in ledger}
    if len(produced) != len(ledger):
        raise ValueError("duplicate Group in the source ledger")
    received = {}
    for e in events:
        if e.get("event") == "object" and e.get("alias") == 2 and e["group"] not in received:
            received[e["group"]] = e
    unknown = sorted(set(received) - set(produced))
    if unknown:
        raise ValueError(f"results that the device never produced: {unknown}")
    events_produced = sum(r["passages_new"] for r in ledger)
    events_received = sum(produced[g]["passages_new"] for g in received)
    true_total = max((r["passages_total"] for r in ledger), default=0)
    reported_total = max((produced[g]["passages_total"] for g in received), default=0)
    lags = [received[g]["t"] - produced[g]["capture_t"] for g in received]
    with_events = [received[g]["t"] - produced[g]["capture_t"] for g in received if produced[g]["passages_new"]]
    missing = sorted(set(produced) - set(received))
    during = []
    for start, end in outages:
        during += [g for g in produced if start <= produced[g]["capture_t"] < end]
    images = [e for e in events if e.get("event") in ("object", "fetch_object") and e.get("alias") in (1, 3)]
    return dict(
        schema=1,
        results_produced=len(produced),
        results_received=len(received),
        results_recovered=sum(1 for e in received.values() if e.get("recovered")),
        results_missing=missing,
        result_delivery_ratio=len(received) / len(produced) if produced else None,
        passage_events_produced=events_produced,
        passage_events_received=events_received,
        passage_events_missed=events_produced - events_received,
        passage_event_delivery_ratio=events_received / events_produced if events_produced else None,
        true_total=true_total,
        reported_total=reported_total,
        count_error=true_total - reported_total,
        produced_during_outage=len(during),
        received_of_outage=sum(1 for g in during if g in received),
        result_lag_s=dict(
            p50=percentile(lags, 0.5), p95=percentile(lags, 0.95), max=max(lags, default=None),
            mean=statistics.mean(lags) if lags else None,
        ),
        passage_result_lag_s=dict(p50=percentile(with_events, 0.5), p95=percentile(with_events, 0.95),
                                  max=max(with_events, default=None)),
        images_received=len(images),
        image_bytes_received=sum(e["bytes"] for e in images),
        note="Delivery of what the device produced. Not recall against physical passages.",
    )


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("ledger", type=Path, help="JSON lines: group, capture_t, passages_new, passages_total")
    p.add_argument("events", type=Path, help="events.jsonl of the receiver")
    p.add_argument("--out", type=Path)
    a = p.parse_args()
    load = lambda path: [json.loads(x) for x in path.read_text().splitlines() if x.strip()]  # noqa: E731
    report = evaluate(load(a.ledger), load(a.events))
    text = json.dumps(report, indent=2) + "\n"
    if a.out:
        a.out.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
