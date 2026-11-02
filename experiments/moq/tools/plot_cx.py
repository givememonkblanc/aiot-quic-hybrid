#!/usr/bin/env python3
"""Throughput campaign (cx) in the terms of Chen et al. (IEEE VTC2023-Fall), per situation.

Task latency zeta as Chen's Eqs. (1), (2), (4): the processing stages only, the final result's
transmission and the wait in the backlog left out. Camera task: its inference ([AI_PROFILE] duration).
Helper task: T_comm + T_comp at the helper, from the offload's publication ([MOQ_OFFLOAD] serial arrival)
to the end of the helper's inference (helper.log [HELPER_DET] arrival), both on the laptop clock.
Base = median camera zeta (Chen Fig. 7: constraints at 1x-4x the base latency).

Row 1: tasks with zeta <= T_max per second of the run, over T_max.
Row 2: helper zeta, measured (dots, box) against Chen's prediction size/R + helper inference, with R the
       link rate of the situation (shaper) and with R the rate the device measured ([MOQ_CHEN] bw_Bps).
Row 3: evidence of the counted passages at the laptop within 10 s (eval_transport --camera-only).
Row 4: rotations whose alerts kept order and the 5 s deadline (eval_transport --camera-only).
Rounds are pooled; each dot in rows 3-4 is one run.

usage: plot_cx.py OUT.png MISSION.json RUN...   (MISSION.json: eval_transport.py --camera-only --json)"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

METHODS = [("mbm-floor-a1", "A: camera alone", "A", "#2a78d6"),
           ("mbm-offc-f", "Chen + FIFO queue", "FIFO", "#eb6834"),
           ("mbm-offc-m", "Chen + MoQ, offload first", "MoQ\noff. first", "#eda100"),
           ("mbm-offc-mp", "Chen + MoQ, mission order", "MoQ\nmission", "#1baf7a")]
SITS = ["r1m", "g2l", "g2n", "u0"]
LINK_BPS = {"r1m": 125000, "g2l": 250000, "g2n": 250000, "u0": 2500000}   # campaign shaper, bytes/s
GRID = np.linspace(0, 6, 241)


def chen_tasks(run):
    """-> (camera zetas, helper zetas, measured R, offload bytes, helper inference s, span s); the span
    is the time over which the camera infers (first to last [AI_PROFILE])."""
    cam, pub, size, ts, bw = [], {}, {}, [], []
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        m = re.match(r"(\S+) \[(AI_PROFILE|MOQ_OFFLOAD|MOQ_CHEN)\] (\{.*\})", line)
        if not m:
            continue
        t = float(m.group(1))
        j = json.loads(m.group(3))
        if m.group(2) == "AI_PROFILE":
            ts.append(t)
            cam.append(j["duration_us"] / 1e6)
        elif m.group(2) == "MOQ_CHEN":
            if j.get("bw_Bps"):
                bw.append(j["bw_Bps"])
        elif j.get("result") == 0:
            pub[j["seq"]] = t
            size[j["seq"]] = j["bytes"]
    hz, comp = [], []
    helper = run / "helper.log"
    for line in (helper.read_text(errors="replace").splitlines() if helper.exists() else []):
        if "[HELPER_DET]" not in line:
            continue
        t = float(line.split(" ", 1)[0])
        j = json.loads(line.split(" ", 2)[2])
        if j["group"] in pub and j.get("ran"):
            hz.append(t - pub[j["group"]])
            comp.append((j["end_us"] - j["start_us"]) / 1e6)
    return (np.array(cam), np.array(hz), float(np.median(bw)) if bw else None,
            float(np.median(list(size.values()))) if size else None,
            float(np.median(comp)) if comp else None, max(ts) - min(ts))


def main():
    out, mission_json, runs = Path(sys.argv[1]), Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]]
    mission = {r["run"]: r for r in json.loads(mission_json.read_text())}
    curves, base, ev, al = defaultdict(list), [], defaultdict(list), defaultdict(list)
    hz, pred = defaultdict(list), defaultdict(list)
    for run in runs:
        m = re.match(r"\w+?-(\w+)-(.+)-r\d+$", run.name)
        man = run / "manifest.json"
        if not m or not man.exists() or '"complete"' not in man.read_text():
            continue
        sit, meth = m.groups()
        cam, h, bw, size, comp, span = chen_tasks(run)
        base += list(cam)
        allz = np.sort(np.concatenate([cam, h]))
        curves[sit, meth].append(np.searchsorted(allz, GRID, side="right") / span)
        if len(h):
            hz[sit, meth] += list(h)
            pred[sit, meth].append((size, bw, comp))
        r = mission.get(run.name)
        if r:
            ev[sit, meth].append(r["evidence_in_time"] / max(r["passages"], 1))
            al[sit, meth].append(r["complete_deadline"] / max(r["paired"], 1))
    b = float(np.median(base))
    sits = [s for s in SITS if any(k[0] == s for k in curves)]
    plt.rcParams.update({"font.size": 9, "axes.edgecolor": "#c3c2b7", "axes.labelcolor": "#3d3d3a",
                         "xtick.color": "#3d3d3a", "ytick.color": "#3d3d3a"})
    fig, ax = plt.subplots(4, len(sits), figsize=(3.8 * len(sits), 11.2), squeeze=False, facecolor="#fcfcfb",
                           gridspec_kw={"height_ratios": [1.5, 1.2, 0.9, 0.9]})
    top = max(float(np.mean(c, axis=0).max()) for c in curves.values()) * 1.1
    ztop = max([max(v) for v in hz.values()] + [4 * b]) * 1.08
    for c, sit in enumerate(sits):
        x = ax[0][c]
        for k in (1, 2, 3, 4):
            x.axvline(k * b, color="#b4b2a8", lw=0.8, ls=(0, (2, 2)))
            x.text(k * b, top, f"{k}×", ha="center", va="bottom", fontsize=7, color="#6b6a62")
        for meth, label, _, col in METHODS:
            if (sit, meth) in curves:
                x.plot(GRID, np.mean(curves[sit, meth], axis=0), color=col, lw=2, label=label)
        x.set_xlim(0, GRID[-1]); x.set_ylim(0, top)
        x.set_title(sit, fontsize=10, color="#1a1a19", pad=14)
        x.set_xlabel("T_max (s)")
        if c == 0:
            x.set_ylabel("tasks with ζ ≤ T_max per s")
        # row 2: helper zeta, measured vs Chen's prediction
        z = ax[1][c]
        for k in (1, 2, 3, 4):
            z.axhline(k * b, color="#e8e7e1", lw=0.8, ls=(0, (2, 2)))
        for i, (meth, _, short, col) in enumerate(METHODS):
            v = hz.get((sit, meth))
            if not v:
                continue
            z.boxplot([v], positions=[i], widths=0.5, showfliers=False, patch_artist=True,
                      boxprops=dict(facecolor=col, edgecolor=col, alpha=0.35), medianprops=dict(color=col, lw=2),
                      whiskerprops=dict(color=col), capprops=dict(color=col))
            jit = (np.random.default_rng(0).random(len(v)) - 0.5) * 0.3
            z.plot(i + jit, v, "o", ms=2.5, color=col, alpha=0.6)
            size = np.median([p[0] for p in pred[sit, meth]])
            comp = np.median([p[2] for p in pred[sit, meth]])
            bws = [p[1] for p in pred[sit, meth] if p[1]]
            p_link = size / LINK_BPS[sit] + comp
            z.plot([i - 0.32, i + 0.32], [p_link] * 2, color="#1a1a19", lw=1.6)
            if bws:
                p_meas = size / np.median(bws) + comp
                z.plot([i - 0.32, i + 0.32], [p_meas] * 2, color="#1a1a19", lw=1.2, ls=(0, (3, 2)))
        z.set_xticks(range(len(METHODS))); z.set_xticklabels([m[2] for m in METHODS], fontsize=8)
        z.set_xlim(-0.6, len(METHODS) - 0.4); z.set_ylim(0, ztop)
        if c == 0:
            z.set_ylabel("helper task ζ (s)")
        # rows 3-4: mission
        for r, (data, name) in enumerate(((ev, "evidence within 10 s"), (al, "rotations: alerts in order, 5 s")), 2):
            y = ax[r][c]
            for i, (meth, _, short, col) in enumerate(METHODS):
                vals = data.get((sit, meth), [])
                if not vals:
                    continue
                y.bar(i, np.mean(vals), width=0.62, color=col, edgecolor="#fcfcfb", linewidth=2)
                y.plot([i] * len(vals), vals, "o", ms=4, mfc="#fcfcfb", mec="#3d3d3a", mew=0.8)
                y.text(i, np.mean(vals) + 0.03, f"{np.mean(vals):.0%}", ha="center", va="bottom", fontsize=8,
                       color="#3d3d3a")
            y.set_ylim(0, 1.18); y.set_xticks(range(len(METHODS))); y.set_xticklabels([m[2] for m in METHODS], fontsize=8)
            y.set_yticks([0, 0.5, 1]); y.set_yticklabels(["0", "50%", "100%"])
            if c == 0:
                y.set_ylabel(name)
    for x in ax.flat:
        x.set_facecolor("#fcfcfb"); x.grid(axis="y", color="#e8e7e1", lw=0.6); x.set_axisbelow(True)
        x.spines[["top", "right"]].set_visible(False)
    h, l = ax[0][0].get_legend_handles_labels()
    h += [plt.Line2D([], [], color="#1a1a19", lw=1.6), plt.Line2D([], [], color="#1a1a19", lw=1.2, ls=(0, (3, 2)))]
    l += ["Chen's prediction, R = link rate", "Chen's prediction, R = rate the device measured"]
    fig.legend(h, l, loc="lower center", ncol=3, frameon=False, fontsize=8)
    n = {k: len(v) for k, v in curves.items()}
    fig.text(0.01, 0.997, f"ζ as Chen et al. Eqs. (1),(2),(4): processing stages, result return excluded. Base = median camera ζ "
             f"{b * 1000:.0f} ms; dashed: 1×–4× base. Runs per cell: {min(n.values())}–{max(n.values())}",
             va="top", fontsize=8, color="#6b6a62")
    fig.tight_layout(rect=(0, 0.045, 1, 0.98))
    fig.savefig(out, dpi=160, facecolor=fig.get_facecolor())
    for (sit, meth), cs in sorted(curves.items()):
        y = np.mean(cs, axis=0)
        v = hz.get((sit, meth), [])
        print(f"{sit:4s} {meth:12s} n {len(cs)} | " + " ".join(f"{k}x {y[np.searchsorted(GRID, k * b)]:.2f}" for k in (1, 2, 3, 4))
              + (f" | helper ζ p50 {np.median(v):.2f}" if v else "")
              + f" | evidence {np.mean(ev.get((sit, meth), [np.nan])):.0%} alerts {np.mean(al.get((sit, meth), [np.nan])):.0%}")


if __name__ == "__main__":
    main()
