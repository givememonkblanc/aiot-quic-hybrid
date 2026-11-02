#!/usr/bin/env python3
"""MBM and WMM apart: the 2x2 factorial (MBM on/off x WMM on/off) of campaign_matrix11.sh, by hop.

  method            MBM (device window + relay alert policy)   WMM (ESP32 AC_VO results, AC_BE images)
  prio-to-low       off                                        off
  prio-to-low-wmm   off                                        on
  mbm-floor         on                                         off
  mbm-floor-wmm     on                                         on

WMM acts only on the radio of the ESP32, the first hop; the relay and its subscriber links are on
the host. The delay of an alert at subscriber j is split at the relay (analyze_matrix.run_alerts):

  hop 1   production on the device -> result at the relay    (device MBM window, WMM)
  hop 2   relay -> subscriber j                               (relay policy; not WMM)

Per situation the script gives, per method (runs pooled), and as factorial effects with 95%
run-level bootstrap intervals (runs resampled within each cell):

  H1>tau1   share of up alerts whose hop 1 exceeds tau1 (100 ms, the device budget) or is missing
  H1>tau    the same for tau (200 ms)
  H1 p95    hop-1 delay p95
  H2 p95    hop-2 delay p95, over the subscribers with all tracks
  MVR       end-to-end MVR_up(tau), mean over the subscribers, and worst subscriber
  hop2 share of the end-to-end violations whose hop 2 is the larger part

  effect MBM   = mean(mbm cells) - mean(prio cells)
  effect WMM   = mean(wmm cells) - mean(no-wmm cells)
  interaction  = (mbm-floor-wmm - mbm-floor) - (prio-to-low-wmm - prio-to-low)

A WMM effect on hop 2 should be zero; it is reported as a check.

usage: analyze_wmm_split.py --prefix mw0 [--out results/wmm_split.json]"""
import argparse
import collections
import json
import random
import statistics
import time
from pathlib import Path

from analyze_matrix import run_alerts

CELLS = {"prio-to-low": (0, 0), "prio-to-low-wmm": (0, 1), "mbm-floor": (1, 0), "mbm-floor-wmm": (1, 1)}
TAU, TAU1 = 0.2, 0.1


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q / 100 * len(v)))] if v else None


def run_record(run, offset):
    subs = run_alerts(run, offset)
    first = {}
    for s in subs.values():
        for a in s["alerts"]:
            if a["kind"] == "up":
                first.setdefault(a["group"], a["hops"][0] if a["hops"] else None)
    h1 = list(first.values())
    hop2, e2e = [], {}
    for name, s in subs.items():
        up = [a for a in s["alerts"] if a["kind"] == "up"]
        bad = [a for a in up if a["delay"] is None or a["delay"] > TAU]
        e2e[name] = dict(n=len(up), bad=len(bad),
                         hop2_bad=sum(1 for a in bad if a["hops"] and a["hops"][1] > a["hops"][0]))
        if s["tracks"] == "all":
            hop2 += [a["hops"][1] for a in up if a["hops"]]
    return dict(h1=h1, hop2=hop2, e2e=e2e)


def cell_stats(runs):
    h1 = [x for r in runs for x in r["h1"]]
    hop2 = [x for r in runs for x in r["hop2"]]
    names = runs[0]["e2e"].keys()
    mvr = {n: sum(r["e2e"][n]["bad"] for r in runs) / max(1, sum(r["e2e"][n]["n"] for r in runs)) for n in names}
    bad = sum(r["e2e"][n]["bad"] for r in runs for n in names)
    return dict(
        h1_tau1=sum(1 for x in h1 if x is None or x > TAU1) / len(h1),
        h1_tau=sum(1 for x in h1 if x is None or x > TAU) / len(h1),
        h1_p95_ms=1000 * pct([x for x in h1 if x is not None], 95),
        h2_p95_ms=1000 * pct(hop2, 95) if hop2 else None,
        mvr_mean=statistics.mean(mvr.values()), mvr_worst=max(mvr.values()),
        worst=max(mvr, key=mvr.get),
        hop2_share=sum(r["e2e"][n]["hop2_bad"] for r in runs for n in names) / bad if bad else None,
        runs=len(runs), alerts=len(h1))


KEYS = ("h1_tau1", "h1_tau", "h1_p95_ms", "h2_p95_ms", "mvr_mean", "mvr_worst")


def effects(cells):
    m = {k: cells[c] for c, k in ((c, CELLS[c]) for c in cells)}
    out = {}
    for key in KEYS:
        v = {k: m[k][key] for k in m}
        if None in v.values():
            continue
        out[key] = dict(mbm=(v[1, 0] + v[1, 1] - v[0, 0] - v[0, 1]) / 2,
                        wmm=(v[0, 1] + v[1, 1] - v[0, 0] - v[1, 0]) / 2,
                        interaction=(v[1, 1] - v[1, 0]) - (v[0, 1] - v[0, 0]))
    return out


def boot(by_cell, n=2000, seed=1):
    rng = random.Random(seed)
    draws = collections.defaultdict(list)
    for _ in range(n):
        cells = {c: cell_stats([rng.choice(runs) for _ in runs]) for c, runs in by_cell.items()}
        for key, e in effects(cells).items():
            for name, v in e.items():
                draws[key, name].append(v)
    return {f"{k}:{n}": (pct(v, 2.5), pct(v, 97.5)) for (k, n), v in draws.items()}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", type=Path, default=Path("results"))
    p.add_argument("--prefix", required=True)
    p.add_argument("--out", type=Path)
    a = p.parse_args()
    offset = time.time() - time.monotonic()
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for run in sorted(a.results.glob(a.prefix + "-*")):
        mf, man = run / "matrix.json", run / "manifest.json"
        if not (mf.exists() and man.exists() and (run / "links.json").exists()):
            continue
        m = json.loads(mf.read_text())
        if m["prefix"] != a.prefix or m["method"] not in CELLS or json.loads(man.read_text()).get("status") != "complete":
            continue
        by[m["situation"]][m["method"]].append(run_record(run, offset))
    report = {}
    for sit, by_cell in sorted(by.items()):
        cells = {c: cell_stats(r) for c, r in by_cell.items()}
        print(f"\n== {sit}")
        print(f"{'method':16} {'runs':>4} {'alerts':>6} {'H1>100':>7} {'H1>200':>7} {'H1p95':>6} {'H2p95':>6} "
              f"{'MVRmean':>7} {'MVRworst':>8} {'worst':>6} {'hop2%':>6}")
        for c in CELLS:
            if c in cells:
                s = cells[c]
                hop2 = "" if s["hop2_share"] is None else f"{100 * s['hop2_share']:5.0f}%"
                print(f"{c:16} {s['runs']:4} {s['alerts']:6} {100*s['h1_tau1']:6.1f}% {100*s['h1_tau']:6.1f}% "
                      f"{s['h1_p95_ms']:6.0f} {s['h2_p95_ms'] or 0:6.0f} {100*s['mvr_mean']:6.1f}% "
                      f"{100*s['mvr_worst']:7.1f}% {s['worst']:>6} {hop2:>6}")
        report[sit] = dict(cells=cells)
        if len(cells) == 4:
            eff, ci = effects(cells), boot(by_cell)
            scale = lambda k: 1 if k.endswith("_ms") else 100
            print(f"{'effect':10} " + " ".join(f"{k:>24}" for k in eff))
            for name in ("mbm", "wmm", "interaction"):
                print(f"{name:10} " + " ".join(
                    f"{scale(k)*eff[k][name]:+7.1f} [{scale(k)*ci[f'{k}:{name}'][0]:+6.1f},{scale(k)*ci[f'{k}:{name}'][1]:+6.1f}]"
                    for k in eff))
            report[sit].update(effects=eff, ci=ci)
    if a.out:
        a.out.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
