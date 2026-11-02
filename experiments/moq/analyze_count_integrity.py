#!/usr/bin/env python3
"""Count known to the receiver against the count of the device, over time, for device runs.

Two ways of reading the count are compared on the same arrivals: the cumulative count
carried by the latest result, and the sum of the passage events of the results that arrived.
Also reported: when each result was produced and whether it arrived, the time the receiver
needs after an outage until its count equals that of the device, and the time of the
reconnection."""

import argparse
import json
import time
from pathlib import Path

from analyze_mission_device import serial_records
from count_metrics import incorrect_time

TOTAL, NEW = "14336", "14338"


def analyze(run, clock_offset):
    manifest = json.loads((run / "manifest.json").read_text())
    events = [json.loads(x) for x in (run / "events.jsonl").read_text().splitlines() if x.strip()]
    subscribed = [e["t"] for e in events if e["event"] == "subscribed"]
    epoch = max(subscribed[:2])
    produced, passages, config = {}, {}, {}
    for t, tag, data in serial_records(run / "serial.log"):
        if tag == "MOQ_AI_PRODUCED":
            produced[data["seq"]] = dict(t=t - clock_offset - epoch, enqueued=bool(data.get("enqueued")))
        elif tag == "MOQ_PASSAGE":
            passages[data["seq"]] = passages.get(data["seq"], 0) + 1
        elif tag == "MOQ_PASSAGE_CONFIG":
            config = data
    total, truth = 0, []
    for group in sorted(produced):
        total += passages.get(group, 0)
        produced[group].update(new=passages.get(group, 0), total=total)
        truth.append((produced[group]["t"], total))
    arrivals = {}
    for e in events:
        if e["event"] == "object" and e.get("alias") == 2 and e["group"] not in arrivals and e["group"] in produced:
            arrivals[e["group"]] = dict(t=e["t"] - epoch, total=e["properties"][TOTAL], new=e["properties"][NEW],
                                        recovered=bool(e.get("recovered")))
    for group, p in produced.items():
        p["arrived"] = arrivals.get(group, {}).get("t")
        p["recovered"] = arrivals.get(group, {}).get("recovered", False)
    # The count known to the receiver after every arrival, read in both ways.
    known, cumulative, summed = [], 0, 0
    for a in sorted(arrivals.values(), key=lambda a: a["t"]):
        cumulative = max(cumulative, a["total"])
        summed += a["new"]
        known.append((a["t"], cumulative, summed))
    outages = [(o["start_s"], o["end_s"]) for o in manifest.get("outages", [])]
    catch_up = None
    if outages and known:
        end = outages[-1][1]
        # The first arrival after the outage whose cumulative count equals the count of the device at that time.
        for t, c, _ in known:
            if t >= end and c == max([v for u, v in truth if u <= t], default=0):
                catch_up = t - end
                break
    span = max([t for t, _ in truth] + [a["t"] for a in arrivals.values()], default=0.0)
    wrong_cumulative = incorrect_time(truth, [(t, c) for t, c, _ in known], 0.0, span)
    wrong_summed = incorrect_time(truth, [(t, s) for t, _, s in known], 0.0, span)
    second = [t - epoch for t in subscribed[2:3]]
    lost = [p["t"] for p in produced.values() if p["arrived"] is None]
    return dict(
        run=run.name, config=config, outages=outages, results_produced=len(produced), results_arrived=len(arrivals),
        device_count=total, count_cumulative=known[-1][1] if known else 0, count_summed=known[-1][2] if known else 0,
        error_cumulative=total - (known[-1][1] if known else 0), error_summed=total - (known[-1][2] if known else 0),
        catch_up_s=catch_up, reconnected_s=second[0] if second else None,
        span_s=span, incorrect_cumulative_s=wrong_cumulative, incorrect_summed_s=wrong_summed,
        results=[dict(group=g, **p) for g, p in sorted(produced.items())], lost_produced_s=lost,
        source_age_ms=[e["properties"].get("41452") for e in events if e["event"] == "object" and e.get("alias") == 2
                       and not e.get("recovered")],
    )


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("runs", nargs="+", type=Path)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    offset = time.time() - time.monotonic()
    reports = []
    for run in a.runs:
        try:
            reports.append(analyze(run, offset))
        except (OSError, ValueError, KeyError, IndexError) as failure:
            print(f"{run.name}: unusable, {failure!r}")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(dict(schema=1, reports=reports), indent=1) + "\n")
    for r in reports:
        catch = "-" if r["catch_up_s"] is None else f"{r['catch_up_s']:.1f}"
        print(f"{r['run']:40s} {r['results_arrived']:2d}/{r['results_produced']:2d} device {r['device_count']:2d} "
              f"cumulative {r['count_cumulative']:2d} summed {r['count_summed']:2d} catch-up {catch} s")


if __name__ == "__main__":
    main()
