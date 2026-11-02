#!/usr/bin/env python3
"""Final table of the 10/13 campaign (PREFIX fx): per situation and method over its rounds, the throughput
(input offload: helper results per s at the laptop; split inference: tasks with Chen's zeta <= 1.5 x base
per s, camera's own plus split), the evidence of the counted passages at the laptop within 10 s
(eval_transport --camera-only), and the alerts' latency (camera results, arrival minus capture, from the
run's fastest). Tests pool the passages (evidence: Fisher's exact test) and the results (latency:
Mann-Whitney U) of a method against MoQ MBM in the same cell.
usage: final_fx.py MISSION.json RUN..."""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.stats import fisher_exact, mannwhitneyu

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plot_throughput import latencies  # noqa: E402

LABEL = {"mbm-floor-a1": "A (camera alone)", "mqtt-offc": "input offload, MQTT", "mbm-offc-f": "input offload, MoQ FIFO",
         "mbm-offc-mp": "input offload, MoQ MBM", "mqtt-offs": "split, MQTT", "mbm-offs-f": "split, MoQ FIFO",
         "mbm-offs-md": "split, MoQ MBM"}
ORDER = list(LABEL)
MBM = {"mqtt-offc": "mbm-offc-mp", "mbm-offc-f": "mbm-offc-mp", "mqtt-offs": "mbm-offs-md", "mbm-offs-f": "mbm-offs-md"}


def run_stats(p):
    ai, sp = [], {}
    for line in (p / "serial.log").read_text(errors="replace").splitlines():
        m = re.match(r"(\S+) \[(AI_PROFILE|AI_SPLIT)\] (\{.*\})", line)
        if m:
            t, j = float(m.group(1)), json.loads(m.group(3))
            if m.group(2) == "AI_PROFILE":
                ai.append((t, j["duration_us"] / 1e6))
            else:
                sp[j["seq"]] = (t, j["decode_us"] / 1e6)
    h = {}
    hl = p / "helper.log"
    for line in (hl.read_text(errors="replace").splitlines() if hl.exists() else []):
        if "[HELPER_DET]" in line:
            j = json.loads(line.split(" ", 2)[2])
            h.setdefault(j["group"], float(line.split(" ", 1)[0]))
    span = ai[-1][0] - ai[0][0]
    cam, hres, _, s2, _ = latencies(p)
    if sp:
        z = [sp[g][1] + (h[g] - sp[g][0]) for g in sp if g in h]
        tp = (sum(d <= 1.5 for _, d in ai) + sum(x <= 1.5 for x in z)) / span
    elif h:
        tp = len(hres) / s2
    else:
        tp = sum(d <= 1.5 for _, d in ai) / span
    return tp, cam


def main():
    mission = {r["run"]: r for r in json.loads(Path(sys.argv[1]).read_text())}
    cells = defaultdict(lambda: dict(tp=[], ev_ok=0, ev_n=0, ev_runs=[], lat=[], p99=[]))
    for run in map(Path, sys.argv[2:]):
        m = re.match(r"fx-(\w+)-(.+)-r\d$", run.name)
        man = run / "manifest.json"
        if not m or m.group(2) not in LABEL or not man.exists() or '"complete"' not in man.read_text():
            continue
        c = cells[m.groups()]
        tp, lat = run_stats(run)
        r = mission.get(run.name, {})
        c["tp"].append(tp)
        c["ev_ok"] += r.get("evidence_in_time", 0)
        c["ev_n"] += r.get("passages", 0)
        c["ev_runs"].append(r.get("evidence_in_time", 0) / max(r.get("passages", 1), 1))
        c["lat"] += list(lat)
        c["p99"].append(float(np.percentile(lat, 99)))
    print(f"{'situation / method':34s} n  throughput /s     evidence <=10 s       alert latency p50 / p99 (s)   vs MoQ MBM: evidence p, latency p")
    for sit in ("r1m", "g2l"):
        for meth in ORDER:
            c = cells.get((sit, meth))
            if not c:
                continue
            tp = np.array(c["tp"])
            ev = c["ev_ok"] / max(c["ev_n"], 1)
            lat = np.array(c["lat"])
            test = ""
            ref = cells.get((sit, MBM.get(meth, "")))
            if ref:
                pe = fisher_exact([[c["ev_ok"], c["ev_n"] - c["ev_ok"]], [ref["ev_ok"], ref["ev_n"] - ref["ev_ok"]]])[1]
                pl = mannwhitneyu(lat, np.array(ref["lat"]), alternative="two-sided")[1]
                test = f"p={pe:.1e}, p={pl:.1e}"
            print(f"{sit + ' ' + LABEL[meth]:34s} {len(tp)}  {tp.mean():.2f} ± {tp.std(ddof=1) if len(tp) > 1 else 0:.2f}"
                  f"      {ev:5.1%} ({c['ev_ok']}/{c['ev_n']})   {np.median(lat):.2f} / {np.percentile(lat, 99):.2f}"
                  f" (run p99 {np.mean(c['p99']):.2f})   {test}")


if __name__ == "__main__":
    main()
