#!/usr/bin/env python3
"""Relay runs (RELAY_EXPERIMENT.md): per subscriber, the alert delay from the end of the
inference on the device to the arrival at the subscriber, split by the outages of the link of
that subscriber, and the part of each alert on the first hop (device to relay) and on the
second (relay to subscriber), from the relay log on the same clock.

Each subscriber becomes a run of its own for analyze_evidence_utility.analyze: the directory
<run>/an-<name> holds its events, the serial record of the device, and a manifest whose outages
are those of its link.

usage: analyze_relay.py --prefix rl1 --out results/rl1.json"""
import argparse
import collections
import json
import random
import re
import shutil
import time
from pathlib import Path

from analyze_evidence_utility import analyze

NAME = re.compile(r"(?P<prefix>.+?)-(?P<policy>fifo|prio|prio-to|alert|mqtt|imquic)-r(?P<round>\d+)$")


def events(path):
    return [json.loads(x) for x in path.read_text().splitlines() if x.strip()]


def subscriber_run(run, sub):
    """<run>/an-<name>: the subscriber's events, its link's outages after its subscription."""
    src, dst = run / f"sub-{sub['name']}", run / f"an-{sub['name']}"
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir()
    rows = events(src / "events.jsonl")
    subs = [r for r in rows if r["event"] == "subscribed"]
    # the epoch of the analysis is the second subscription of the first session; a subscriber
    # of the results alone has one
    first_lost = next((r["t"] for r in rows if r["event"] == "session_lost"), float("inf"))
    first = [r for r in subs if r["t"] < first_lost]
    if len(first) == 1:
        rows.insert(rows.index(first[0]) + 1, dict(first[0]))
        first = first * 2
    epoch = max(r["t"] for r in first[:2])
    (dst / "events.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
    for f in ("serial.log", "clock.json"):
        if (run / f).exists():
            (dst / f).symlink_to((run / f).resolve())
    m = json.loads((run / "manifest.json").read_text())
    m["outage_actions"] = [dict(action=a["action"], actual_s=a["t"] - epoch) for a in sub["actions"]]
    (dst / "manifest.json").write_text(json.dumps(m, indent=1) + "\n")
    return dst, epoch


def boot(runs, stat, n=2000, seed=1):
    rng = random.Random(seed)
    out = sorted(v for v in (stat([x for r in [rng.choice(runs) for _ in runs] for x in r]) for _ in range(n)) if v is not None)
    return (out[int(0.025 * len(out))], out[int(0.975 * len(out))]) if out else (None, None)


def pct(v, q):
    v = sorted(v)
    return v[round(q / 100 * (len(v) - 1))] if v else None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", type=Path, default=Path("results"))
    p.add_argument("--prefix", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--alert-ms", type=float, default=100.0)
    a = p.parse_args()
    offset = time.time() - time.monotonic()
    runs, cells = [], collections.defaultdict(list)
    for run in sorted(a.results.glob(a.prefix + "-*")):
        m = NAME.match(run.name)
        if not m or m["prefix"] != a.prefix or not (run / "links.json").exists():
            continue
        manifest = json.loads((run / "manifest.json").read_text())
        if manifest.get("status") != "complete":
            print(f"{run.name}: {manifest.get('status')}")
            continue
        links = json.loads((run / "links.json").read_text())
        # outages of the first hop (device to relay), on the clock of the run: its epoch is the
        # second subscription of the relay to the device
        subscribed = [e["t"] for e in events(run / "events.jsonl") if e["event"] == "subscribed"]
        run_epoch = max(subscribed[:2])
        acts = [run_epoch + x["actual_s"] for x in manifest.get("outage_actions", [])]
        first_spans = list(zip(acts[::2], acts[1::2]))
        relay = events(run / "relay.jsonl")
        up = {r["group"]: r["t"] for r in relay if r["ev"] == "up_object" and r["alias"] == 2}
        up_bytes = sum(r.get("bytes", 0) for r in relay if r["ev"] == "up_object")
        for sub in links["subscribers"]:
            d, epoch = subscriber_run(run, sub)
            try:
                r = analyze(d, offset, [0.115, 0.491, 1.0], 10.0, [0.4, 0.5, 0.1])
            except (OSError, ValueError, KeyError, IndexError) as failure:
                print(f"{run.name}/{sub['name']}: unusable, {failure!r}")
                continue
            spans = [(s, e) for s, e in zip(*[iter([x["actual_s"] for x in json.loads((d / "manifest.json").read_text())["outage_actions"]])] * 2)]
            # An alert meets the link of the subscriber when it reaches the relay: it belongs to an
            # outage when it reached the relay while that link was blocked, and its delay is then
            # counted from the end of the outage. Otherwise its delay runs from its production.
            up_s, down_s, hops, first_down = [], [], [], []
            for (t, arrival), g in zip(r["event_alerts"], r["event_alert_groups"]):
                if arrival is None:
                    continue
                # produced while the first hop was blocked: from the return of the first hop
                fs = next((sp for sp in first_spans if sp[0] <= epoch + t < sp[1]), None)
                if fs:
                    first_down.append(epoch + arrival - fs[1])
                    continue
                enters = up[g] - epoch if g in up else t
                span = next((sp for sp in spans if sp[0] <= enters < sp[1]), None)
                if span:
                    down_s.append(arrival - span[1])
                    continue
                up_s.append(arrival - t)
                if g in up:
                    hops.append(dict(group=g, first=up[g] - (epoch + t), second=epoch + arrival - up[g]))
            r["alert_up_s"], r["alert_after_outage_s"], r["alert_after_first_outage_s"] = up_s, down_s, first_down
            # neighbour devices received by this subscriber: alerts from production (the ledger of the
            # model, on the same monotonic clock) to arrival, within the 120 s of the run
            nb = links.get("neighbours") or {}
            nb_up, nb_lost = [], 0
            if sub["name"] in (nb.get("subs") or {}):
                for k in range(1, nb.get("n", 0) + 1):
                    # the production of a neighbour result is its capture-time property (120, ms of
                    # the monotonic clock) as it reached its relay; property 122 holds its new passages
                    rlog, ev = run / f"relay-nb{k}.jsonl", run / f"nb{k}-{sub['name']}" / "events.jsonl"
                    if not rlog.exists() or not ev.exists():
                        continue
                    seen = {}
                    for e in events(ev):
                        if e["event"] == "object" and e.get("alias") == 2:
                            seen.setdefault(e["group"], e["t"])
                    for o in events(rlog):
                        if o["ev"] != "up_object" or o["alias"] != 2 or not o["properties"].get("122"):
                            continue
                        made = o["properties"]["120"] / 1000
                        if epoch + 5 <= made < epoch + 115:
                            if o["group"] in seen:
                                nb_up.append(seen[o["group"]] - made)
                            else:
                                nb_lost += 1
            r["neighbour_alert_s"], r["neighbour_lost"] = nb_up, nb_lost
            r.update(policy=m["policy"], round=int(m["round"]), subscriber=sub["name"], link=sub["link"],
                     tracks=sub["tracks"], hops=hops, outages=spans,
                     budget_ms=links.get("budget_ms", 100), up_bytes=up_bytes)
            for k in ("objects", "event_times"):
                r.pop(k, None)
            runs.append(r)
            cells[(m["policy"], sub["name"])].append(r)
    rows = []
    for (policy, name), c in sorted(cells.items()):
        up = [r["alert_up_s"] for r in c]
        allv = [x for v in up for x in v]
        down = [x for r in c for x in r["alert_after_outage_s"]]
        lo, hi = boot(up, lambda v: pct(v, 95)) if len(c) > 1 else (None, None)
        hops = [h for r in c for h in r["hops"]]
        rows.append(dict(
            policy=policy, subscriber=name, link=c[0]["link"], tracks=c[0]["tracks"], runs=len(c),
            alerts=sum(len(r["event_alerts"]) for r in c), lost=sum(r["event_lost"] for r in c),
            up_n=len(allv), up_p50_ms=1000 * pct(allv, 50) if allv else None, up_p95_ms=1000 * pct(allv, 95) if allv else None,
            up_p95_ci_ms=[1000 * lo, 1000 * hi] if lo is not None else None,
            over_ms=100 * sum(1 for x in allv if x > a.alert_ms / 1000) / len(allv) if allv else None,
            out_n=len(down), out_p50_ms=1000 * pct(down, 50) if down else None, out_p95_ms=1000 * pct(down, 95) if down else None,
            first_hop_p50_ms=1000 * pct([h["first"] for h in hops], 50) if hops else None,
            second_hop_p50_ms=1000 * pct([h["second"] for h in hops], 50) if hops else None,
            second_hop_p95_ms=1000 * pct([h["second"] for h in hops], 95) if hops else None,
            first_hop_p95_ms=1000 * pct([h["first"] for h in hops], 95) if hops else None,
            second_hop_p95_ci_ms=[1000 * x for x in boot([[h["second"] for h in r["hops"]] for r in c], lambda v: pct(v, 95))]
            if len(c) > 1 else None,
            second_hop_over=100 * sum(1 for h in hops if h["second"] > c[0]["budget_ms"] / 1000) / len(hops) if hops else None,
            budget_ms=c[0]["budget_ms"], up_kB=sum(r["up_bytes"] for r in c) / len(c) / 1000,
            out_all=sorted(down),
            nb_n=sum(len(r["neighbour_alert_s"]) for r in c), nb_lost=sum(r["neighbour_lost"] for r in c),
            nb_p50_ms=1000 * pct([x for r in c for x in r["neighbour_alert_s"]], 50) if any(r["neighbour_alert_s"] for r in c) else None,
            nb_p95_ms=1000 * pct([x for r in c for x in r["neighbour_alert_s"]], 95) if any(r["neighbour_alert_s"] for r in c) else None,
            first_out_all=sorted(x for r in c for x in r["alert_after_first_outage_s"]),
            E=sum(r["E"] for r in c) / len(c)))
    for r in rows:
        print(f"{r['policy']:8s} {r['subscriber']:5s} {r['link']:6s} runs {r['runs']} alerts {r['alerts']} lost {r['lost']} "
              f"up p50 {r['up_p50_ms'] or 0:.0f} p95 {r['up_p95_ms'] or 0:.0f} >{a.alert_ms:.0f} {r['over_ms'] or 0:.0f}% "
              f"after {r['out_n']}: {r['out_p50_ms'] or 0:.0f} hop1 {r['first_hop_p50_ms'] or 0:.0f} "
              f"hop2 {r['second_hop_p50_ms'] or 0:.0f}/{r['second_hop_p95_ms'] or 0:.0f} E {r['E']:.2f}"
              + (f" | hop1 p95 {r['first_hop_p95_ms']:.0f}" if r['first_hop_p95_ms'] else "")
              + (f" | first-hop outage n {len(r['first_out_all'])} p50 {1000 * pct(r['first_out_all'], 50):.0f}" if r['first_out_all'] else "")
              + (f" | neighbours n {r['nb_n']} lost {r['nb_lost']} p50 {r['nb_p50_ms'] or 0:.0f} p95 {r['nb_p95_ms'] or 0:.0f}" if r['nb_n'] or r['nb_lost'] else ""))
    a.out.write_text(json.dumps(dict(rows=rows, runs=runs), default=str) + "\n")


if __name__ == "__main__":
    main()
