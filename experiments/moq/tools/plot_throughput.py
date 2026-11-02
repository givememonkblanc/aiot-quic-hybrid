#!/usr/bin/env python3
"""Inference throughput within a latency bound (Chen et al., VTC2023-Fall: the throughput of stacking
tasks subject to the restricted time T_max of one task), per situation and method, beside the mission.

A task is one captured frame that gets inferred; its latency runs from its capture to the arrival of its
result at the laptop. The device's capture clock is mapped to the laptop's as the smallest arrival minus capture over
the camera's results (as eval_transport.py), so a latency counts from the fastest result of the run: it
compares transports and methods, not absolute times. Throughput within T: distinct frames whose first result (the
camera's or the helper's) arrived within T, per second of capture. Vertical lines: 1x-4x the base
latency (median camera inference, [AI_PROFILE]).

usage: plot_throughput.py OUT.png RUN..."""
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

COLOURS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"]
SITS = ["r1m", "r256", "g2l", "g2n", "u0"]
GRID = np.linspace(0, 8, 321)


def latencies(run):
    """-> (camera latencies, helper latencies, {frame: first latency}, capture span s, inference durations)."""
    recs = []
    for line in (run / "sub-remote" / "events.jsonl").open():
        r = json.loads(line)
        if r.get("event") not in ("object", "fetch_object"):
            continue
        if "helper" in r and r["helper"].get("capture_ms"):
            recs.append(("helper", r["t"], int(r["helper"]["capture_ms"]), None))
        elif r.get("result") and "120" in r.get("properties", {}):
            p = r["properties"]
            recs.append(("camera", r["t"], int(p["120"]), int(p.get("41452", 0))))
    # the clocks are mapped by the smallest arrival minus capture of the camera's results, as
    # eval_transport.py does: the age-at-publish property (41452) is set by the MOQT transport only, and a
    # mapping on it would shift the MQTT baseline by about one inference
    off = min(t - c / 1000 for k, t, c, a in recs if k == "camera")
    first, cam, hlp = {}, [], []
    for k, t, c, _ in recs:
        d = t - off - c / 1000
        (cam if k == "camera" else hlp).append(d)
        first[c] = min(first.get(c, np.inf), d)
    caps = [c for k, _, c, _ in recs if k == "camera"]
    dur = []
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        if "[AI_PROFILE] " in line:
            try:
                dur.append(json.loads(line[line.index("{"):])["duration_us"] / 1e6)
            except ValueError:
                pass
    return cam, hlp, first, (max(caps) - min(caps)) / 1000, dur


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("runs", nargs="+", type=Path)
    a = ap.parse_args()
    curves, nruns, base, frames = defaultdict(list), defaultdict(int), [], defaultdict(list)
    methods = []
    for run in a.runs:
        m = re.match(r"\w+?-(\w+)-(.+)-r\d+$", run.name)
        man = run / "manifest.json"
        if not m or not man.exists() or '"complete"' not in man.read_text():
            continue
        sit, meth = m.group(1), m.group(2)
        cam, hlp, first, span, dur = latencies(run)
        base += dur
        lat = np.sort(np.fromiter(first.values(), float))
        curves[sit, meth].append(np.searchsorted(lat, GRID, side="right") / span)
        frames[sit, meth].append((len(cam), len(hlp), span))
        nruns[sit, meth] += 1
        if meth not in methods:
            methods.append(meth)
    sits = [s for s in SITS if any(k[0] == s for k in curves)]
    if not sits:
        sys.exit("no runs")
    b = float(np.median(base))
    colour = {m: COLOURS[i % len(COLOURS)] for i, m in enumerate(methods)}
    plt.rcParams.update({"font.size": 9, "axes.edgecolor": "#c3c2b7", "axes.labelcolor": "#3d3d3a",
                         "xtick.color": "#3d3d3a", "ytick.color": "#3d3d3a"})
    fig, ax = plt.subplots(1, len(sits), figsize=(3.6 * len(sits), 3.8), sharey=True, squeeze=False,
                           facecolor="#fcfcfb")
    for c, sit in enumerate(sits):
        x = ax[0][c]
        x.set_facecolor("#fcfcfb")
        x.grid(axis="y", color="#e8e7e1", lw=0.6)
        for k in (1, 2, 3, 4):
            x.axvline(k * b, color="#b4b2a8", lw=0.8, ls=(0, (2, 2)))
            x.text(k * b, 1.0, f"{k}×", transform=x.get_xaxis_transform(), ha="center", va="bottom",
                   fontsize=7, color="#6b6a62")
        for meth in methods:
            if (sit, meth) not in curves:
                continue
            y = np.mean(curves[sit, meth], axis=0)
            x.plot(GRID, y, color=colour[meth], lw=2, label=meth)
        x.set_xlim(0, 8)
        x.spines[["top", "right"]].set_visible(False)
        x.set_title(sit, fontsize=10, color="#1a1a19", pad=14)
        x.set_xlabel("latency bound T_max (s)")
        if c == 0:
            x.set_ylabel("inferences within T_max per s")
    top = max(float(np.mean(cs, axis=0).max()) for cs in curves.values())
    for x in ax.flat:
        x.set_ylim(0, top * 1.08)
    h, l = ax[0][0].get_legend_handles_labels()
    for x in ax.flat[1:]:
        for hh, ll in zip(*x.get_legend_handles_labels()):
            if ll not in l:
                h.append(hh); l.append(ll)
    fig.legend(h, l, loc="lower center", ncol=min(len(l), 4), frameon=False, fontsize=8)
    fig.text(0.01, 0.995, f"Base latency (one camera inference) {b * 1000:.0f} ms; dashed: 1×–4× base",
             va="top", fontsize=8, color="#6b6a62")
    fig.tight_layout(rect=(0, 0.1, 1, 0.96))
    fig.savefig(a.out, dpi=160, facecolor=fig.get_facecolor())
    print(f"base latency {b * 1000:.0f} ms")
    for (sit, meth), cs in sorted(curves.items()):
        y = np.mean(cs, axis=0)
        at = " ".join(f"{k}x {y[np.searchsorted(GRID, k * b)]:.2f}" for k in (1, 2, 3, 4))
        fr = frames[sit, meth]
        print(f"{sit:4s} {meth:16s} runs {nruns[sit, meth]} | camera {np.mean([f[0] / f[2] for f in fr]):.2f}/s "
              f"helper {np.mean([f[1] / f[2] for f in fr]):.2f}/s | within T_max (/s): {at} | all {y[-1]:.2f}")


if __name__ == "__main__":
    main()
