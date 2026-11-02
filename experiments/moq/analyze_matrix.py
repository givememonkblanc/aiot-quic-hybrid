#!/usr/bin/env python3
"""Method matrix (tools/campaign_matrix.sh): the mission of every alert at every subscriber.

An alert is a result that reports a passage. Its production is the end of its inference on the
device clock, mapped to the host (analyze_evidence_utility.analyze). At subscriber j the alert
  - is lost when it never arrived;
  - is an "up" alert when the first hop was up at its production and the link of j was up when
    it reached the relay; its delay runs from its production;
  - is an "outage" alert otherwise; its delay runs from the return of the link that was down
    (the recovery delay).
The mission is violated at j by an alert that is lost, or whose delay exceeds the budget tau.

  MVR_up(tau)   share of the up alerts that are lost or later than tau          (primary)
  MVR_rec(tau)  the same for the outage alerts, from the return of the link
  lost          alerts that never arrived, of all alerts
  E             image quality at j (analyze_evidence_utility)
AIoT metrics (an alert is the result of an inference on the device, and its image is the evidence
on which a person verifies it):
  A1 verifiable  share of the up alerts that arrive within tau AND whose evidence, at any
                 representation (the floor), arrives within the evidence deadline of its capture
                 (subscribers of all tracks)
  A2 event delay the delay of an alert from the capture of its image (the event), which holds the
                 inference: p50/p95, and the share above --tau-event-ms
  A3 inference   duration of the inferences of the run (AI_PROFILE), p50/p95; compared with the
                 runs without evidence (prio-to-tau) of the same situation it is the slowing of the
                 inference by the communication
A cell (situation, method) reports every subscriber and the worst subscriber of the results.
Intervals: 95% bootstrap over the runs of the cell (the runs are the independent units), and the
Wilson upper bound of the pooled share (with no violation, about 3/n).

With --floor every representation has quality 1 (the mission needs an image at the low
representation or above, and nothing more): E is the share of the passages with an image in time.

usage: analyze_matrix.py --prefix mx1 --out results/mx1.json [--tau-ms 200] [--floor]"""
import argparse
import collections
import json
import math
import random
import time
from pathlib import Path

from analyze_evidence_utility import analyze
from analyze_relay import events, subscriber_run

TAUS_MS = [25, 50, 75, 100, 150, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 5000]
QUALITIES, DEADLINE_S, WEIGHTS = [0.115, 0.491, 1.0], 10.0, [0.4, 0.5, 0.1]


def wilson_upper(k, n, z=1.96):
    if n == 0:
        return None
    p = k / n
    return (p + z * z / (2 * n) + z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / (1 + z * z / n)


def share(alerts, tau):
    """Violations of tau among alerts given as delays in seconds (None: lost)."""
    return sum(1 for d in alerts if d is None or d > tau)


def boot_share(runs, tau, n=2000, seed=1):
    rng = random.Random(seed)
    out = []
    for _ in range(n):
        pick = [rng.choice(runs) for _ in runs]
        tot = sum(len(r) for r in pick)
        if tot:
            out.append(sum(share(r, tau) for r in pick) / tot)
    out.sort()
    return [out[int(0.025 * len(out))], out[int(0.975 * len(out))]] if out else None


def pct(v, q):
    v = sorted(v)
    return v[round(q / 100 * (len(v) - 1))] if v else None


def run_alerts(run, offset):
    """Per subscriber: the alerts of the run as (kind, delay or None, group)."""
    links = json.loads((run / "links.json").read_text())
    manifest = json.loads((run / "manifest.json").read_text())
    subscribed = [e["t"] for e in events(run / "events.jsonl") if e["event"] == "subscribed"]
    run_epoch = max(subscribed[:2])
    acts = [run_epoch + x["actual_s"] for x in manifest.get("outage_actions", [])]
    first_spans = list(zip(acts[::2], acts[1::2]))
    up = {}
    for r in events(run / "relay.jsonl"):
        if r.get("ev") == "up_object" and r["alias"] == 2:
            up.setdefault(r["group"], r["t"])
    out = {}
    for sub in links["subscribers"]:
        d, epoch = subscriber_run(run, sub)
        r = analyze(d, offset, QUALITIES, DEADLINE_S, WEIGHTS)
        capture = {e["group"]: e["capture"] for e in r["event_times"]}
        evidence = {}
        for e in events(d / "events.jsonl"):
            if e["event"] in ("object", "fetch_object") and e.get("alias") == 3 and "126" in e.get("properties", {}):
                evidence.setdefault(e["group"], e["t"] - epoch)
        spans = [(a["t"] - epoch, b["t"] - epoch) for a, b in zip(sub["actions"][::2], sub["actions"][1::2])]
        alerts = []
        for (t, arrival), g in zip(r["event_alerts"], r["event_alert_groups"]):
            fs = next((sp for sp in first_spans if sp[0] <= epoch + t < sp[1]), None)
            enters = up[g] - epoch if g in up else t
            ss = None if fs else next((sp for sp in spans if sp[0] <= enters < sp[1]), None)
            if fs:
                kind, start = "outage", fs[1] - epoch
            elif ss:
                kind, start = "outage", ss[1]
            else:
                kind, start = "up", t
            hop = None
            if kind == "up" and arrival is not None and g in up:
                hop = [up[g] - (epoch + t), epoch + arrival - up[g]]
            ev = evidence.get(g)
            alerts.append(dict(group=g, kind=kind, delay=None if arrival is None else arrival - start, hops=hop,
                               event_delay=None if arrival is None or g not in capture else arrival - capture[g],
                               evidence=None if sub["tracks"] != "all" else
                               (ev is not None and g in capture and ev - capture[g] <= DEADLINE_S)))
        out[sub["name"]] = dict(link=sub["link"], tracks=sub["tracks"], alerts=alerts, E=r["E"] if sub["tracks"] == "all" else None,
                                images=r["evidence_in_time"] if sub["tracks"] == "all" else None)
    return out


def inference(run):
    """Durations of the inferences of a run, seconds (AI_PROFILE of the serial record)."""
    out = []
    for line in (run / "serial.log").read_text(errors="ignore").splitlines():
        if "[AI_PROFILE]" in line:
            out.append(json.loads(line.split("] ", 1)[1])["duration_us"] / 1e6)
    return out


def summarise(runs, tau, tau_event=1.2):
    """runs: per run, the list of alert dicts of one subscriber."""
    up = [[a["delay"] for a in r if a["kind"] == "up"] for r in runs]
    rec = [[a["delay"] for a in r if a["kind"] == "outage"] for r in runs]
    pooled_up = [x for v in up for x in v]
    pooled_rec = [x for v in rec for x in v]
    arrived = [x for x in pooled_up if x is not None]
    hops = [a["hops"] for r in runs for a in r if a["hops"]]
    n_all = sum(len(r) for r in runs)
    up_alerts = [a for r in runs for a in r if a["kind"] == "up"]
    verifiable = [a for a in up_alerts if a["evidence"] is not None]
    ev_delay = [a["event_delay"] for r in runs for a in r if a["kind"] == "up" and a["event_delay"] is not None]
    return dict(
        a1_verifiable=sum(1 for a in verifiable if a["evidence"] and a["delay"] is not None and a["delay"] <= tau) / len(verifiable)
        if verifiable else None,
        a1_evidence_only=sum(1 for a in verifiable if a["evidence"]) / len(verifiable) if verifiable else None,
        a2_event_p50_ms=1000 * pct(ev_delay, 50) if ev_delay else None,
        a2_event_p95_ms=1000 * pct(ev_delay, 95) if ev_delay else None,
        a2_event_over=(sum(1 for x in ev_delay if x > tau_event) + sum(1 for a in up_alerts if a["delay"] is None)) / len(up_alerts)
        if up_alerts else None,
        runs=len(runs), alerts=n_all, lost=sum(1 for r in runs for a in r if a["delay"] is None),
        up_n=len(pooled_up), mvr_up=share(pooled_up, tau) / len(pooled_up) if pooled_up else None,
        mvr_up_ci=boot_share(up, tau) if len(runs) > 1 else None,
        mvr_up_wilson=wilson_upper(share(pooled_up, tau), len(pooled_up)),
        rec_n=len(pooled_rec), mvr_rec=share(pooled_rec, tau) / len(pooled_rec) if pooled_rec else None,
        rec_p50_ms=1000 * pct([x for x in pooled_rec if x is not None], 50) if any(x is not None for x in pooled_rec) else None,
        rec_p95_ms=1000 * pct([x for x in pooled_rec if x is not None], 95) if any(x is not None for x in pooled_rec) else None,
        p50_ms=1000 * pct(arrived, 50) if arrived else None, p95_ms=1000 * pct(arrived, 95) if arrived else None,
        p99_ms=1000 * pct(arrived, 99) if arrived else None, max_ms=1000 * max(arrived) if arrived else None,
        first_hop_p95_ms=1000 * pct([h[0] for h in hops], 95) if hops else None,
        second_hop_p95_ms=1000 * pct([h[1] for h in hops], 95) if hops else None,
        curve_up={str(t): share(pooled_up, t / 1000) / len(pooled_up) for t in TAUS_MS} if pooled_up else None,
        curve_rec={str(t): share(pooled_rec, t / 1000) / len(pooled_rec) for t in TAUS_MS} if pooled_rec else None,
    )


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", type=Path, default=Path("results"))
    p.add_argument("--prefix", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--tau-ms", type=float, default=200.0)
    p.add_argument("--floor", action="store_true", help="every representation has quality 1")
    p.add_argument("--tau-event-ms", type=float, default=1200.0, help="budget of an alert from the capture (A2)")
    a = p.parse_args()
    if a.floor:
        QUALITIES[:] = [1.0, 1.0, 1.0]
    tau = a.tau_ms / 1000
    offset = time.time() - time.monotonic()
    cells = collections.defaultdict(lambda: collections.defaultdict(list))
    infer = collections.defaultdict(list)
    images = collections.defaultdict(lambda: collections.defaultdict(list))
    skipped = []
    for run in sorted(a.results.glob(a.prefix + "-*")):
        meta = run / "matrix.json"
        if not meta.exists() or not (run / "links.json").exists():
            continue
        m = json.loads(meta.read_text())
        if m["prefix"] != a.prefix:
            continue
        manifest = json.loads((run / "manifest.json").read_text())
        if manifest.get("status") != "complete":
            skipped.append(dict(run=run.name, status=manifest.get("status")))
            continue
        try:
            subs = run_alerts(run, offset)
        except (OSError, ValueError, KeyError, IndexError) as failure:
            skipped.append(dict(run=run.name, status=f"unusable: {failure!r}"))
            continue
        infer[(m["situation"], m["method"])].extend(inference(run))
        for name, s in subs.items():
            cells[(m["situation"], m["method"])][name].append(s["alerts"])
            if s["E"] is not None:
                images[(m["situation"], m["method"])][name].append(s["E"])
    rows = []
    for (sit, method), by_sub in sorted(cells.items()):
        subs = {name: summarise(runs, tau, a.tau_event_ms / 1000) for name, runs in by_sub.items()}
        for name in subs:
            e = images[(sit, method)].get(name)
            subs[name]["E"] = sum(e) / len(e) if e else None
        valid = [s for s in subs.values() if s["mvr_up"] is not None]
        worst = max(valid, key=lambda s: s["mvr_up"]) if valid else None
        a1 = [s["a1_verifiable"] for s in subs.values() if s["a1_verifiable"] is not None]
        inf = infer[(sit, method)]
        rows.append(dict(situation=sit, method=method, subscribers=subs,
                         worst_subscriber=next((k for k, v in subs.items() if v is worst), None),
                         mvr_up=worst["mvr_up"] if worst else None, mvr_up_ci=worst["mvr_up_ci"] if worst else None,
                         mvr_up_wilson=worst["mvr_up_wilson"] if worst else None,
                         lost=sum(s["lost"] for s in subs.values()),
                         mvr_rec=max((s["mvr_rec"] for s in subs.values() if s["mvr_rec"] is not None), default=None),
                         a1_verifiable_min=min(a1) if a1 else None,
                         a2_event_p95_ms=max((s["a2_event_p95_ms"] for s in subs.values() if s["a2_event_p95_ms"] is not None), default=None),
                         a2_event_over_max=max((s["a2_event_over"] for s in subs.values() if s["a2_event_over"] is not None), default=None),
                         a3_infer_p50_ms=1000 * pct(inf, 50) if inf else None, a3_infer_p95_ms=1000 * pct(inf, 95) if inf else None,
                         E=sum(s["E"] for s in subs.values() if s["E"] is not None) /
                         max(1, sum(1 for s in subs.values() if s["E"] is not None))))
    a.out.write_text(json.dumps(dict(prefix=a.prefix, tau_ms=a.tau_ms, taus_ms=TAUS_MS, rows=rows, skipped=skipped), indent=1) + "\n")
    print(f"{'sit':4} {'method':16} {'runs':>4} {'MVR_up%':>8} {'CI':>15} {'wilson%':>8} {'lost':>5} {'MVR_rec%':>9} {'E':>5} "
          f"{'A1%':>5} {'A2p95':>6} {'A2>%':>5} {'A3p50':>6} {'A3p95':>6}  worst")
    for r in rows:
        s = r["subscribers"][r["worst_subscriber"]] if r["worst_subscriber"] else {}
        ci = r["mvr_up_ci"]
        g = lambda v, k=1, f=".0f": "    -" if v is None else format(k * v, f)
        print(f"{r['situation']:4} {r['method']:16} {s.get('runs', 0):4} "
              f"{100 * r['mvr_up'] if r['mvr_up'] is not None else float('nan'):8.2f} "
              f"{'' if not ci else f'[{100 * ci[0]:.1f},{100 * ci[1]:.1f}]':>15} "
              f"{100 * r['mvr_up_wilson'] if r['mvr_up_wilson'] is not None else float('nan'):8.2f} {r['lost']:5} "
              f"{100 * r['mvr_rec'] if r['mvr_rec'] is not None else float('nan'):9.1f} {r['E']:5.2f} "
              f"{g(r['a1_verifiable_min'], 100):>5} {g(r['a2_event_p95_ms']):>6} {g(r['a2_event_over_max'], 100):>5} "
              f"{g(r['a3_infer_p50_ms']):>6} {g(r['a3_infer_p95_ms']):>6}  {r['worst_subscriber']}")
    for s in skipped:
        print("skipped", s)


if __name__ == "__main__":
    main()
