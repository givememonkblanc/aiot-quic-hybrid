#!/usr/bin/env python3
"""Share of passages served within a latency bound, per situation and method (Chen et al., VTC2023-Fall,
Fig. 7: latency constraints as multiples of the base latency of one local inference).

Top row: the direction task of each counted passage. Helper: the arrival of the helper's result at the
laptop on the device capture clock (laptop arrival minus the least camera lag, the earliest the camera
itself alerts on a frame) minus the capture of the passage frame; a task without a result never counts.
Camera alone (model): the task run on the camera ahead of its count, one inference each, from the
passage alert (serial [MOQ_PASSAGE]); it takes that many inference slots from the count.
Bottom row: the evidence of each passage at the laptop from its crossing (eval_transport.evidence).
Vertical lines: 1x-4x the base latency (median camera inference, [AI_PROFILE] of the A runs), and the
evidence deadline of the firmware (10 s).

usage: plot_task_latency.py OUT.png RUN... [--template T]"""
import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import analyze_rotations as A  # noqa: E402
import eval_transport as E  # noqa: E402

METHODS = {  # image -> (label, colour, line style); fixed order
    "mbm-floor-a1": ("Camera alone (model)", "#2a78d6", (0, (4, 2))),
    "mbm-offt4-all": ("Helper, FIFO send queue", "#eb6834", "-"),
    "mbm-offt4-all-m": ("Helper, MoQ offload first", "#eda100", (0, (1, 1.5))),
    "mbm-offt4-all-mp": ("Helper, MoQ mission order", "#1baf7a", "-"),
}
SITS = ["r1m", "g2l", "g2n", "u0"]
GRID = np.linspace(0, 12, 481)


def serial(run, tags):
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        m = re.match(r"(\S+) \[(\w+)\] (\{.*\})", line)
        if m and m.group(2) in tags:
            try:
                yield float(m.group(1)), m.group(2), json.loads(m.group(3))
            except ValueError:
                pass


def helper_latency(run):
    """-> [latency s or inf] per counted passage."""
    pas, task = {}, {}
    for _, tag, j in serial(run, {"MOQ_PASSAGE", "MOQ_TASK"}):
        if tag == "MOQ_PASSAGE":
            pas[j["id"]] = j["seq"]
        else:
            task[j["passage"]] = j["seq"]
    cap, lag, hlp = {}, [], {}
    for line in (run / "sub-remote" / "events.jsonl").open():
        r = json.loads(line)
        if r.get("event") not in ("object", "fetch_object"):
            continue
        if "helper" in r:
            hlp.setdefault(r["helper"].get("group"), r["t"])
        elif r.get("result") and "120" in r.get("properties", {}):
            c = int(r["properties"]["120"]) / 1000
            cap[r["result"]["seq"]] = c
            lag.append(r["t"] - c)
    off = min(lag)
    out = []
    for pid, seq in pas.items():
        g = task.get(pid)
        out.append(hlp[g] - off - cap[seq] if g in hlp and seq in cap else np.inf)
    return out


def local_latency(run):
    """-> ([latency s] per passage, inference durations s) for the camera running the task itself; each
    task takes one inference from the count."""
    dur, pas = [], []
    for t, tag, j in serial(run, {"AI_PROFILE", "MOQ_PASSAGE"}):
        if tag == "AI_PROFILE":
            dur.append(j["duration_us"] / 1e6)
        else:
            pas.append(t)
    d, free, out = float(np.median(dur)), 0.0, []
    for t in pas:
        s = max(t, free)
        free = s + d
        out.append(free - t)
    return out, dur


def evidence_latency(run, tp):
    ev = run / "sub-remote" / "events.jsonl"
    return [np.inf if d is None else d for _, d in E.evidence(run, ev, tp)]


def share(lat):
    a = np.sort(np.asarray(lat, float))
    return np.searchsorted(a, GRID, side="right") / max(len(a), 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--template", type=Path, default=Path("results/trace-s3-long-r2/slot_template.json"))
    a = ap.parse_args()
    tp = A.Template(a.template)
    task, evid, base, nruns = defaultdict(list), defaultdict(list), [], defaultdict(int)
    taken = []   # share of the camera's inferences the tasks would take from the count
    for run in a.runs:
        m = re.match(r"\w+?-(\w+)-(.+)-r\d+$", run.name)
        man = run / "manifest.json"
        if not m or m.group(2) not in METHODS or not man.exists() or '"complete"' not in man.read_text():
            continue
        sit, meth = m.group(1), m.group(2)
        if meth == "mbm-floor-a1":
            lat, dur = local_latency(run)
            base += dur
            taken.append(len(lat) / max(len(dur), 1))
        else:
            lat = helper_latency(run)
        task[sit, meth] += lat
        evid[sit, meth] += evidence_latency(run, tp)
        nruns[sit, meth] += 1
    sits = [s for s in SITS if any(k[0] == s for k in task)]
    if not sits:
        sys.exit("no runs")
    b = float(np.median(base)) if base else 1.08
    plt.rcParams.update({"font.size": 9, "axes.edgecolor": "#c3c2b7", "axes.labelcolor": "#3d3d3a",
                         "xtick.color": "#3d3d3a", "ytick.color": "#3d3d3a"})
    fig, ax = plt.subplots(2, len(sits), figsize=(3.4 * len(sits), 6.2), sharey=True, squeeze=False,
                           facecolor="#fcfcfb")
    for c, sit in enumerate(sits):
        for r, (data, title) in enumerate(((task, "direction task"), (evid, "evidence"))):
            x = ax[r][c]
            x.set_facecolor("#fcfcfb")
            x.grid(axis="y", color="#e8e7e1", lw=0.6)
            for k in ([1, 2, 3, 4] if r == 0 else []):
                x.axvline(k * b, color="#b4b2a8", lw=0.8, ls=(0, (2, 2)))
                x.text(k * b, 1.02, f"{k}×", ha="center", va="bottom", fontsize=7, color="#6b6a62")
            if r == 1:
                x.axvline(10, color="#b4b2a8", lw=0.8, ls=(0, (2, 2)))
                x.text(10, 1.02, "10 s", ha="center", va="bottom", fontsize=7, color="#6b6a62")
            for meth, (label, col, ls) in METHODS.items():
                lat = data.get((sit, meth))
                if not lat:
                    continue
                y = share(lat)
                if meth == "mbm-floor-a1" and taken:
                    lo, hi = f"{min(taken):.0%}", f"{max(taken):.0%}"
                    label = f"{label}: takes {lo if lo == hi else lo + '-' + hi} of the count's inferences"
                x.plot(GRID, y, color=col, lw=2, ls=ls, label=f"{label} (n={nruns[sit, meth]})")
                x.plot(GRID[-1], y[-1], "o", ms=4, color=col)
            x.set_xlim(0, 8 if r == 0 else 12)
            x.set_ylim(0, 1.0)
            x.spines[["top", "right"]].set_visible(False)
            if c == 0:
                x.set_ylabel(f"share of passages, {title}")
            if r == 1:
                x.set_xlabel("latency bound (s)")
            if r == 0:
                x.set_title(sit, fontsize=10, color="#1a1a19", pad=14)
    h, l = [], []
    for x in ax.flat:
        for hh, ll in zip(*x.get_legend_handles_labels()):
            key = ll.split(" (n=")[0]
            if key not in [q.split(" (n=")[0] for q in l]:
                h.append(hh); l.append(key)
    fig.legend(h, l, loc="lower center", ncol=2, frameon=False, fontsize=8)
    fig.text(0.01, 0.995, f"Base latency (one camera inference) {b * 1000:.0f} ms; dashed: 1×–4× base (Chen et al. Fig. 7)",
             va="top", fontsize=8, color="#6b6a62")
    fig.tight_layout(rect=(0, 0.08, 1, 0.97))
    fig.savefig(a.out, dpi=160, facecolor=fig.get_facecolor())
    for (sit, meth), lat in sorted(task.items()):
        y = share(lat)
        at = {k: y[np.searchsorted(GRID, k * b)] for k in (1, 2, 3, 4)}
        ev = share(evid[sit, meth])[np.searchsorted(GRID, 10.0)]
        print(f"{sit:4s} {meth:20s} runs {nruns[sit, meth]} passages {len(lat):4d} | task within "
              + " ".join(f"{k}x {v:.2f}" for k, v in at.items()) + f" | evidence within 10 s {ev:.2f}")


if __name__ == "__main__":
    main()
