#!/usr/bin/env python3
"""Clairvoyant first hop: the most evidence any sender could bring to the relay in a run.

The oracle knows the run in advance: when each passage is produced, the size of its image, the
rate of the first hop and its outages. It paces its bytes at the rate of the link, so that the
queue of the bottleneck stays empty and every result reaches the relay one result transmission
after its production: it meets any budget tau above the delay of the link, MVR = 0. It chooses
for each passage the representation of its image (none, low, medium, high) that maximises E, the
mean over the passages of the quality of the best image that reaches the relay within the
evidence deadline (10 s from the capture, as analyze_evidence_utility.py).

Feasibility: an image is released at the production of its passage (the image is known to show
a passage only then) and due at its capture plus the deadline. On one link served preemptively,
a choice of images can be delivered iff, for every interval between a release and a deadline,
the bytes of the images released and due within it do not exceed what the link carries in it
(rate x efficiency x time the link is up). E* is the solution of that integer program (scipy
milp, HiGHS). When the solver stops at its time limit, E* is the best choice it found and E*_bound
the bound it proved: the most evidence lies between the two. The link carries EFFICIENCY of its
rate as payload (QUIC, UDP and IP headers).

The methods are taken at the same place, the relay (the up_object records of relay.jsonl):
  E_relay    E of the images that reached the relay
  MVR_hop1   share of the alerts produced while the first hop was up whose result reached the
             relay later than tau after its production, or never
A method is efficient when E_relay is near E* at MVR_hop1 near 0. In s3, s6 and s7 the capacity
of the first hop is lower than its rate (loss, contention); E* uses the rate and is a loose bound.

With --floor the mission needs an image at the low representation or above (400 x 300, on which
the screws can be counted and placed) and values nothing more: every representation has quality 1
and E is the share of the passages with an image in time.

usage: analyze_oracle.py --prefix mx1 [--prefix mx1f] --out results/oracle.json [--tau-ms 200] [--floor]"""
import argparse
import collections
import json
import statistics
import time
from pathlib import Path

import numpy as np
from scipy.optimize import Bounds, LinearConstraint, milp

from analyze_evidence_utility import analyze
from analyze_mission_device import serial_records
from analyze_relay import events, subscriber_run

QUALITY = {0: 1.0, 1: 0.491, 2: 0.115}          # representation level -> quality
DEADLINE_S = 10.0
EFFICIENCY = 0.95
TIME_LIMIT_S = 180
EXACT = {"s1", "s2", "s4", "s5", "r256", "r1m"}                 # the first hop is the rate and its outages


def up_spans(spans, a, b):
    """Seconds in [a, b] outside the outage spans."""
    down = sum(max(0.0, min(b, e) - max(a, s)) for s, e in spans)
    return max(0.0, b - a - down)


def oracle(jobs, rate_Bps, spans):
    """jobs: dict(release, due, sizes{level: bytes}). Returns E*, its proven bound and the levels."""
    n = len(jobs)
    if n == 0:
        return 1.0, 1.0, []
    levels = [0, 1, 2]
    nv = n * len(levels)
    c = -np.array([QUALITY[l] for _ in jobs for l in levels])
    rows, ub = [], []
    for i in range(n):                              # one image per passage at most
        row = np.zeros(nv)
        row[i * 3:(i + 1) * 3] = 1
        rows.append(row)
        ub.append(1.0)
    releases = sorted({j["release"] for j in jobs})
    dues = sorted({j["due"] for j in jobs})
    for a in releases:
        for b in dues:
            if b <= a:
                continue
            inside = [i for i, j in enumerate(jobs) if j["release"] >= a and j["due"] <= b]
            if not inside:
                continue
            row = np.zeros(nv)
            for i in inside:
                for k, l in enumerate(levels):
                    row[i * 3 + k] = jobs[i]["sizes"][l]
            rows.append(row)
            ub.append(rate_Bps * EFFICIENCY * up_spans(spans, a, b))
    res = milp(c, constraints=LinearConstraint(np.array(rows), -np.inf, np.array(ub)),
               integrality=np.ones(nv), bounds=Bounds(0, 1), options=dict(time_limit=TIME_LIMIT_S))
    if res.x is None:
        raise ValueError(f"oracle: {res.message}")
    x = res.x.round().astype(int)
    chosen = [next((levels[k] for k in range(3) if x[i * 3 + k]), None) for i in range(n)]
    bound = getattr(res, "mip_dual_bound", None)
    return -res.fun / n, (-bound / n if bound is not None and res.status != 0 else -res.fun / n), chosen


def ratios(runs):
    """Bytes of the medium and the low representation per byte of the high one, over the runs."""
    high, by_level = [], collections.defaultdict(list)
    for run in runs:
        for _, tag, d in serial_records(run / "serial.log"):
            if tag == "MOQ_EVIDENCE":
                high.append(d["bytes"])
            elif tag == "MOQ_REPRESENTATION":
                by_level[d["level"]].append(d["bytes"])
    h = statistics.median(high)
    return {0: 1.0, 1: statistics.median(by_level[1]) / h, 2: statistics.median(by_level[2]) / h}, h


def run_case(run, offset, ratio, median_high, tau):
    m = json.loads((run / "matrix.json").read_text())
    links = json.loads((run / "links.json").read_text())
    sub = next(s for s in links["subscribers"] if s["tracks"] == "all")
    d, epoch = subscriber_run(run, sub)
    r = analyze(d, offset, [QUALITY[2], QUALITY[1], QUALITY[0]], DEADLINE_S, [0.4, 0.5, 0.1])
    manifest = json.loads((run / "manifest.json").read_text())
    subscribed = [e["t"] for e in events(run / "events.jsonl") if e["event"] == "subscribed"]
    run_epoch = max(subscribed[:2])
    acts = [run_epoch + x["actual_s"] - epoch for x in manifest.get("outage_actions", [])]
    spans = list(zip(acts[::2], acts[1::2]))
    hop = m["first_hop"]
    rate_Bps = float(hop[hop.index("--rate-kbit") + 1]) * 1000 / 8
    size = {g: b for _, tag, x in serial_records(run / "serial.log") if tag == "MOQ_EVIDENCE"
            for g, b in [(x["group"], x["bytes"])]}
    production = dict(zip(r["event_alert_groups"], (p for p, _ in r["event_alerts"])))
    relay = events(run / "relay.jsonl")
    end = max(x["t"] for x in relay) - epoch
    jobs = []
    for e in r["event_times"]:
        g = e["group"]
        high = size.get(g, median_high)
        jobs.append(dict(group=g, release=production.get(g, e["known"]), due=min(end, e["capture"] + DEADLINE_S),
                         capture=e["capture"], sizes={l: high * ratio[l] for l in (0, 1, 2)}))
    e_star, e_bound, chosen = oracle(jobs, rate_Bps, spans)
    # the method at the relay
    best, first = {}, {}
    for x in relay:
        if x.get("ev") != "up_object":
            continue
        if x["alias"] == 2:
            first.setdefault(x["group"], x["t"] - epoch)
        elif x["alias"] == 3 and "126" in x.get("properties", {}):
            j = next((j for j in jobs if j["group"] == x["group"]), None)
            if j and x["t"] - epoch <= j["capture"] + DEADLINE_S:
                best[x["group"]] = max(best.get(x["group"], 0.0), QUALITY[x["properties"]["126"]])
    e_relay = statistics.mean(best.get(j["group"], 0.0) for j in jobs) if jobs else 1.0
    up = [(g, p) for g, p in production.items() if not any(s <= p < e for s, e in spans)]
    late = sum(1 for g, p in up if g not in first or first[g] - p > tau)
    return dict(run=run.name, situation=m["situation"], method=m["method"], round=m["round"], rate_kbit=rate_Bps * 8 / 1000,
                outages=spans, passages=len(jobs), E_star=e_star, E_star_bound=e_bound, E_relay=e_relay,
                share_of_oracle=e_relay / e_star if e_star else None,
                oracle_levels={str(k): chosen.count(k) for k in (0, 1, 2, None)},
                mvr_hop1=late / len(up) if up else None, up_alerts=len(up), exact=m["situation"] in EXACT)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", type=Path, default=Path("results"))
    p.add_argument("--prefix", action="append", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--tau-ms", type=float, default=200.0)
    p.add_argument("--floor", action="store_true", help="every representation has quality 1")
    a = p.parse_args()
    if a.floor:
        QUALITY.update({0: 1.0, 1: 1.0, 2: 1.0})
    offset = time.time() - time.monotonic()
    runs = []
    for prefix in a.prefix:
        for run in sorted(a.results.glob(prefix + "-*")):
            mf, man = run / "matrix.json", run / "manifest.json"
            if mf.exists() and man.exists() and json.loads(mf.read_text())["prefix"] == prefix \
                    and json.loads(man.read_text()).get("status") == "complete":
                runs.append(run)
    ratio, median_high = ratios(runs)
    cases, skipped = [], []
    for run in runs:
        try:
            cases.append(dict(prefix=json.loads((run / "matrix.json").read_text())["prefix"],
                              **run_case(run, offset, ratio, median_high, a.tau_ms / 1000)))
        except (OSError, ValueError, KeyError, IndexError, StopIteration) as failure:
            skipped.append(dict(run=run.name, why=repr(failure)))
    cells = collections.defaultdict(list)
    for c in cases:
        cells[(c["prefix"], c["situation"], c["method"])].append(c)
    rows = []
    for (prefix, sit, method), cs in sorted(cells.items()):
        mean = lambda k: statistics.mean(c[k] for c in cs if c[k] is not None) if any(c[k] is not None for c in cs) else None
        rows.append(dict(prefix=prefix, situation=sit, method=method, runs=len(cs), E_star=mean("E_star"), E_star_bound=mean("E_star_bound"),
                         E_relay=mean("E_relay"), share_of_oracle=mean("share_of_oracle"), mvr_hop1=mean("mvr_hop1"),
                         exact=sit in EXACT))
    a.out.write_text(json.dumps(dict(tau_ms=a.tau_ms, quality=QUALITY, efficiency=EFFICIENCY, deadline_s=DEADLINE_S, ratio=ratio,
                                     median_high_bytes=median_high, rows=rows, cases=cases, skipped=skipped), indent=1) + "\n")
    print(f"ratios medium {ratio[1]:.3f} low {ratio[2]:.3f} of a high image of {median_high:.0f} B (median)")
    print(f"{'prefix':6} {'sit':4} {'method':16} {'runs':>4} {'MVR_hop1%':>9} {'E_relay':>7} {'E*':>6} {'bound':>6} {'share':>6}")
    for r in rows:
        f = lambda v, k=1: "   -" if v is None else f"{k * v:.2f}" if k == 1 else f"{k * v:.1f}"
        print(f"{r['prefix']:6} {r['situation']:4} {r['method']:16} {r['runs']:4} {f(r['mvr_hop1'], 100):>9} "
              f"{f(r['E_relay']):>7} {f(r['E_star']):>6} {f(r['E_star_bound']):>6} {f(r['share_of_oracle']):>6}{'' if r['exact'] else '  (bound)'}")
    for s in skipped:
        print("skipped", s)


if __name__ == "__main__":
    main()
