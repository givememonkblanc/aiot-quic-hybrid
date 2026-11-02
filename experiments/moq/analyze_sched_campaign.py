#!/usr/bin/env python3
"""Device comparison of evidence scheduling: mission utility by method and link.

Reads the runs `<prefix>-<method>-<link>-r<n>` and writes one row for every method and link
with the mean and the range over the runs. Evidence bytes that served nothing are those of
objects that were reset and those of objects that arrived after the evidence deadline.

The alert columns concern the results that report a passage: the time from the production
of such a result on the device to its arrival at the receiver, the share of them later than
--alert-ms, and the number that never arrived.

On a link whose rate trace has an outage (rate 0), an alert produced during the outage cannot
arrive before the link returns. The alerts are therefore also split: those produced while the
link was up, with their delay, and those produced during an outage, with their delay after
the return of the link (arrival minus the end of the outage)."""

import argparse
import collections
import json
import re
import statistics
import time
from pathlib import Path

from analyze_evidence_utility import analyze
from analyze_mission_device import serial_records

METHODS = ["prio-high", "prio-med", "timeout-high", "timeout-med", "abr", "feasible", "mus-alert", "mus-alert-probe", "mus-1000",
           "mus-nobound", "mus-full", "mus-norisk", "mus-noahead", "mus"]
LINKS = ["64", "96", "128", "256", "512", "wifi", "var", "f128", "f512", "fwifi", "trace"]


def useless_bytes(run, report):
    reset = 0
    for _, tag, data in serial_records(run / "serial.log"):
        if tag == "MOQ_TIMEOUT_RESET" and data.get("alias") == 3:
            reset += data["acked"]
        elif tag == "MOQ_MUS_DONE" and not data["delivered"]:
            reset += data["bytes"]
    return reset, report["image_bytes_late"]


def outages(run):
    """[(start, end)] seconds after the subscription in which the shaped rate was 0, from the
    applied actions of the rate trace, or in which the flow was blocked by an outage."""
    m = json.loads((run / "manifest.json").read_text())
    acts = [x for x in m.get("outage_actions") or [] if x.get("action") == "rate"]
    out = []
    for i, x in enumerate(acts):
        if x["rate_kbit"] == 0:
            end = acts[i + 1]["actual_s"] if i + 1 < len(acts) else float("inf")
            out.append((x["actual_s"], end))
    # outages of --outage: block and unblock
    edges = [x for x in m.get("outage_actions") or [] if x.get("action") in ("block", "unblock") and x.get("actual_s") is not None]
    for x, y in zip(edges, edges[1:]):
        if x["action"] == "block" and y["action"] == "unblock":
            out.append((x["actual_s"], y["actual_s"]))
    return sorted(out)


def split_alerts(r, spans):
    up, down = [], []
    for t, arrival in r["event_alerts"]:
        span = next((s for s in spans if s[0] <= t < s[1]), None)
        if arrival is None:
            continue
        if span:
            down.append(arrival - span[1])
        else:
            up.append(arrival - t)
    return up, down


def pct(v, q):
    return 1000 * v[round(q / 100 * (len(v) - 1))] if v else None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", type=Path, default=Path("results"))
    p.add_argument("--prefix", default="sched-dev-v2")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--alert-ms", type=float, default=100.0)
    p.add_argument("--qualities", type=float, nargs=3, default=[0.115, 0.491, 1.0])
    p.add_argument("--deadline", type=float, default=10.0)
    p.add_argument("--weights", type=float, nargs=3, default=[0.4, 0.5, 0.1])
    a = p.parse_args()
    offset = time.time() - time.monotonic()
    name = re.compile(re.escape(a.prefix) + r"-(?P<method>.+)-(?P<link>" + "|".join(LINKS) + r")-r(?P<run>\d+)$")
    cells, runs = collections.defaultdict(list), []
    for run in sorted(a.results.glob(a.prefix + "-*")):
        m = name.match(run.name)
        if not m or not (run / "manifest.json").exists() or not (run / "serial.log").exists():
            continue
        try:
            r = analyze(run, offset, a.qualities, a.deadline, a.weights)
        except (OSError, ValueError, KeyError, IndexError) as failure:
            print(f"{run.name}: unusable, {failure!r}")
            continue
        if r["status"] != "complete":
            print(f"{run.name}: {r['status']}")
            continue
        r["bytes_reset"], r["bytes_late"] = useless_bytes(run, r)
        r["outages"] = outages(run)
        r["alert_up_s"], r["alert_after_outage_s"] = split_alerts(r, r["outages"])
        r.update(method=m["method"], link=m["link"])
        runs.append(r)
        cells[(m["method"], m["link"])].append(r)
    rows = []
    for method in METHODS:
        for link in LINKS:
            c = cells.get((method, link))
            if not c:
                continue
            row = dict(method=method, link=link, runs=len(c), events=sum(r["events"] for r in c),
                       results=sum(r["results"] for r in c), count_error=sum(abs(r["count_error"]) for r in c),
                       firmware=sorted({r["firmware"] for r in c}))
            for k in ("U", "C", "E", "H", "evidence_in_time"):
                v = [r[k] for r in c]
                row[k] = dict(mean=statistics.mean(v), min=min(v), max=max(v), values=v)
            row["levels"] = {k: sum(r["evidence_by_level"][k] for r in c) for k in ("0", "1", "2")}
            row["best"] = {k: sum(r["evidence_best"][k] for r in c) for k in ("0", "1", "2", "none")}
            delays = sorted(x for r in c for x in r["result_delay_s"])
            row["result_delay_median_s"] = delays[len(delays) // 2] if delays else None
            row["result_delay_p95_s"] = delays[int(0.95 * (len(delays) - 1))] if delays else None
            alerts = sorted(x for r in c for x in r["event_delay_s"])
            row["alert_n"] = len(alerts)
            row["alert_lost"] = sum(r["event_lost"] for r in c)
            for q in (50, 95, 99):
                row[f"alert_p{q}_ms"] = 1000 * alerts[round(q / 100 * (len(alerts) - 1))] if alerts else None
            row["alert_max_ms"] = 1000 * alerts[-1] if alerts else None
            row["alert_miss"] = sum(1 for x in alerts if 1000 * x > a.alert_ms) / len(alerts) if alerts else None
            up = sorted(x for r in c for x in r["alert_up_s"])
            down = sorted(x for r in c for x in r["alert_after_outage_s"])
            row["alert_up"] = dict(n=len(up), p50_ms=pct(up, 50), p95_ms=pct(up, 95), max_ms=pct(up, 100),
                                   miss=sum(1 for x in up if 1000 * x > a.alert_ms) / len(up) if up else None)
            row["alert_after_outage"] = dict(n=len(down), p50_ms=pct(down, 50), p95_ms=pct(down, 95), max_ms=pct(down, 100))
            row["late"] = sum(r["evidence_late"] for r in c)
            row["useless_kB"] = statistics.mean(r["bytes_reset"] + r["bytes_late"] for r in c) / 1000
            row["delivered_kB"] = statistics.mean(r["image_bytes_delivered"] for r in c) / 1000
            rows.append(row)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(dict(schema=1, kind="device", qualities=a.qualities, deadline_s=a.deadline, weights=a.weights,
                                     rows=rows, runs=runs), indent=1) + "\n")
    for link in LINKS:
        if not any(x["link"] == link for x in rows):
            continue
        print(f"\n{link:>4}              runs alerts  p50ms  p95ms  p99ms  max_ms  >{a.alert_ms:.0f}ms lost   E    in time  image kB")
        for r in (x for x in rows if x["link"] == link):
            if not r["alert_n"]:
                print(f"  {r['method']:17s} {r['runs']}  no alerts")
                continue
            print(f"  {r['method']:17s} {r['runs']}  {r['alert_n']:5d} {r['alert_p50_ms']:6.0f} {r['alert_p95_ms']:6.0f} "
                  f"{r['alert_p99_ms']:6.0f} {r['alert_max_ms']:7.0f}  {100 * r['alert_miss']:5.1f}% {r['alert_lost']:4d}  "
                  f"{r['E']['mean']:.3f}  {r['evidence_in_time']['mean']:.2f}  {r['delivered_kB']:8.1f}")

    for link in LINKS:
        sel = [x for x in rows if x["link"] == link and x["alert_after_outage"]["n"]]
        if not sel:
            continue
        print(f"\n{link:>4} split    link up: alerts  p50ms  p95ms  max_ms  >{a.alert_ms:.0f}ms | during outage: alerts, "
              f"after return p50ms  p95ms  max_ms")
        for r in sel:
            u, d = r["alert_up"], r["alert_after_outage"]
            print(f"  {r['method']:17s}      {u['n']:5d} {u['p50_ms']:6.0f} {u['p95_ms']:6.0f} {u['max_ms']:7.0f}  "
                  f"{100 * u['miss']:5.1f}% |       {d['n']:5d}            {d['p50_ms']:6.0f} {d['p95_ms']:6.0f} {d['max_ms']:7.0f}")


if __name__ == "__main__":
    main()
