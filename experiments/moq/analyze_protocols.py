#!/usr/bin/env python3
"""Aggregates a protocol_compare.py suite: delivery, count error, incorrect-count time, recovery."""

import argparse
import collections
import json
import statistics
from pathlib import Path

from count_metrics import incorrect_time, recovery_times


def cell(directory):
    report = json.loads((directory / "report.json").read_text())
    ledger = [json.loads(x) for x in (directory / "ledger.jsonl").read_text().splitlines()]
    events_file = directory / "events.jsonl"
    events = [json.loads(x) for x in events_file.read_text().splitlines()] if events_file.exists() else []
    start = json.loads((directory / "device.json").read_text())["start"]
    produced = {r["group"]: r for r in ledger}
    arrivals = {}
    for e in events:
        if e.get("event") == "object" and e.get("alias") == 2 and e["group"] in produced and e["group"] not in arrivals:
            arrivals[e["group"]] = e
    truth = [(r["capture_t"], r["passages_total"]) for r in ledger]
    known, summed, c, s = [], [], 0, 0
    for e in sorted(arrivals.values(), key=lambda e: e["t"]):
        c = max(c, produced[e["group"]]["passages_total"])
        s += produced[e["group"]]["passages_new"]
        known.append((e["t"], c))
        summed.append((e["t"], s))
    end = max([r["capture_t"] for r in ledger] + [e["t"] for e in arrivals.values()])
    ends = [start + b for _, b in report["outage_trace"]]
    outage_time = sum(b - a for a, b in report["outage_trace"])
    lags = sorted(arrivals[g]["t"] - produced[g]["capture_t"] for g in arrivals)
    calm = sorted(arrivals[g]["t"] - produced[g]["capture_t"] for g in arrivals
                  if not any(start + a - 1.0 <= produced[g]["capture_t"] <= start + b + 10.0 for a, b in report["outage_trace"]))
    pct = lambda v, q: v[min(len(v) - 1, round(q * (len(v) - 1)))] if v else None  # noqa: E731
    return dict(
        policy=report["policy"], seed=report["seed"], queue=report["queue_results"], duration=end - start,
        outage_time=outage_time, outages=len(ends),
        produced=len(produced), received=len(arrivals), recovered=sum(1 for e in arrivals.values() if e.get("recovered")),
        events_produced=truth[-1][1] if truth else 0,
        error_cumulative=(truth[-1][1] if truth else 0) - (known[-1][1] if known else 0),
        error_summed=(truth[-1][1] if truth else 0) - (summed[-1][1] if summed else 0),
        incorrect_cumulative_s=incorrect_time(truth, known, start, end),
        incorrect_summed_s=incorrect_time(truth, summed, start, end),
        recovery_s=recovery_times(truth, known, ends),
        outage_lengths=[b - a for a, b in report["outage_trace"]],
        lag_p50=pct(lags, .5), lag_p95=pct(lags, .95), lag_calm_p50=pct(calm, .5), lag_calm_p95=pct(calm, .95),
        images=sum(1 for e in events if e.get("event") == "object" and e.get("alias") == 3),
        sessions=report["device"].get("sessions"), overflow=report["device"].get("results_overflow", 0),
        expired=report["device"].get("results_expired", 0) + report["device"].get("results_reset", 0),
        errors=report.get("errors", []))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("suite", type=Path)
    a = p.parse_args()
    cells = [cell(d) for d in sorted(a.suite.iterdir()) if (d / "report.json").exists()]
    groups = collections.defaultdict(list)
    for c in cells:
        groups[(c["queue"], c["policy"])].append(c)
    rows = []
    for (queue, policy), g in sorted(groups.items()):
        rec = [x for c in g for x in c["recovery_s"] if x is not None]
        unrecovered = sum(1 for c in g for x in c["recovery_s"] if x is None)
        f = lambda k: sum(c[k] for c in g)  # noqa: E731
        rows.append(dict(
            queue=queue, policy=policy, runs=len(g), produced=f("produced"), received=f("received"),
            recovered=f("recovered"), events=f("events_produced"), error_cumulative=f("error_cumulative"),
            error_summed=f("error_summed"), duration=f("duration"), outage_time=f("outage_time"),
            incorrect_cumulative_s=f("incorrect_cumulative_s"), incorrect_summed_s=f("incorrect_summed_s"),
            recovery_median=statistics.median(rec) if rec else None,
            recovery_p90=sorted(rec)[round(.9 * (len(rec) - 1))] if rec else None, recovery_max=max(rec, default=None),
            recoveries=len(rec), unrecovered=unrecovered,
            lag_calm_p50=statistics.median(c["lag_calm_p50"] for c in g if c["lag_calm_p50"] is not None),
            lag_calm_p95=statistics.median(c["lag_calm_p95"] for c in g if c["lag_calm_p95"] is not None),
            images=f("images"), overflow=f("overflow"), expired=f("expired"),
            runs_with_errors=sum(1 for c in g if c["errors"])))
    (a.suite / "analysis.json").write_text(json.dumps(dict(schema=1, rows=rows, cells=cells), indent=1) + "\n")
    print("queue policy runs delivered  err_cum err_sum  incorrect%(cum/sum)  recovery med/p90/max  calm lag p50/p95  images")
    for r in rows:
        print(f"{r['queue']:5d} {r['policy']:3s} {r['runs']:3d} {r['received']:5d}/{r['produced']:<5d} "
              f"{r['error_cumulative']:4d} {r['error_summed']:5d}   "
              f"{100 * r['incorrect_cumulative_s'] / r['duration']:5.1f} / {100 * r['incorrect_summed_s'] / r['duration']:5.1f}    "
              f"{r['recovery_median'] or 0:5.2f} {r['recovery_p90'] or 0:5.2f} {r['recovery_max'] or 0:5.2f} ({r['unrecovered']} none)   "
              f"{r['lag_calm_p50']:.2f} {r['lag_calm_p95']:.2f}   {r['images']}")


if __name__ == "__main__":
    main()
