#!/usr/bin/env python3
"""Figures of the 10/13 campaign (PREFIX fx), per situation (r1m, g2l), rounds pooled.

fig_throughput: tasks with Chen's task latency zeta <= T_max per second over T_max (1x-4x the base latency,
  the median camera inference). Camera task: its inference; split task: the camera's decode plus the time
  from the split to the helper's result; input-offload task: from the frame's publication to the helper's
  result (plot_cx.chen_tasks). Colour = transport, line = collaboration design.
fig_mission: evidence of the counted passages at the laptop within 10 s (eval_transport --camera-only) and
  the alerts' latency p99 per run (camera results, arrival minus capture from the run's fastest); one dot
  per run, bar = mean.
fig_chen: input offload, the helper task's zeta measured against Chen's prediction size / R + helper
  inference, R = the link rate of the situation (solid) or the rate the device measured (dashed).
usage: plot_final.py OUTDIR MISSION.json RUN..."""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plot_throughput import latencies  # noqa: E402
from plot_cx import chen_tasks, LINK_BPS  # noqa: E402

TRANSPORT = {"A": "#2a78d6", "MQTT": "#eb6834", "MoQ FIFO": "#eda100", "MoQ MBM": "#1baf7a"}
METHODS = [  # (method, transport, design)
    ("mbm-floor-a1", "A", "none"),
    ("mqtt-offs", "MQTT", "split"), ("mbm-offs-f", "MoQ FIFO", "split"), ("mbm-offs-md", "MoQ MBM", "split"),
    ("mqtt-offc", "MQTT", "input"), ("mbm-offc-f", "MoQ FIFO", "input"), ("mbm-offc-mp", "MoQ MBM", "input"),
]
STYLE = {"none": "-", "split": "-", "input": (0, (4, 2))}
SITS = ["r1m", "g2l"]
GRID = np.linspace(0, 6, 241)
SURFACE, INK, MUTED, GRIDC = "#fcfcfb", "#3d3d3a", "#6b6a62", "#e8e7e1"


def split_tasks(run):
    """-> zetas of the split tasks: the camera's decode plus split -> helper result (laptop clock)."""
    sp, h = {}, {}
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        m = re.match(r"(\S+) \[AI_SPLIT\] (\{.*\})", line)
        if m:
            j = json.loads(m.group(2))
            sp[j["seq"]] = (float(m.group(1)), j["decode_us"] / 1e6)
    hl = run / "helper.log"
    for line in (hl.read_text(errors="replace").splitlines() if hl.exists() else []):
        if "[HELPER_DET]" in line:
            j = json.loads(line.split(" ", 2)[2])
            h.setdefault(j["group"], float(line.split(" ", 1)[0]))
    return np.array([sp[g][1] + h[g] - sp[g][0] for g in sp if g in h])


def style_axes(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(axis="y", color=GRIDC, lw=0.6)
    ax.set_axisbelow(True)
    ax.spines[["top", "right"]].set_visible(False)


def main():
    out, mission, runs = Path(sys.argv[1]), json.loads(Path(sys.argv[2]).read_text()), [Path(p) for p in sys.argv[3:]]
    out.mkdir(parents=True, exist_ok=True)
    mission = {r["run"]: r for r in mission}
    curves, ev, p99, zeta, pred = (defaultdict(list) for _ in range(5))
    base = []
    for run in runs:
        m = re.match(r"fx-(\w+)-(.+)-r\d$", run.name)
        man = run / "manifest.json"
        if not m or not man.exists() or '"complete"' not in man.read_text():
            continue
        sit, meth = m.groups()
        if meth not in {x[0] for x in METHODS}:
            continue
        cam, hz, bw, size, comp, span = chen_tasks(run)
        base += list(cam)
        sz = split_tasks(run)
        allz = np.sort(np.concatenate([cam, sz if len(sz) else hz]))
        curves[sit, meth].append(np.searchsorted(allz, GRID, side="right") / span)
        r = mission.get(run.name)
        if r:
            ev[sit, meth].append(r["evidence_in_time"] / max(r["passages"], 1))
        lat, *_ = latencies(run)
        p99[sit, meth].append(float(np.percentile(lat, 99)))
        if meth.endswith("offc") or meth.startswith("mbm-offc"):
            zeta[sit, meth] += list(hz)
            pred[sit, meth].append((size, bw, comp))
    b = float(np.median(base))
    plt.rcParams.update({"font.size": 9, "axes.edgecolor": "#c3c2b7", "axes.labelcolor": INK,
                         "xtick.color": INK, "ytick.color": INK})

    # fig_throughput
    fig, ax = plt.subplots(1, 2, figsize=(9.2, 3.9), sharey=True, facecolor=SURFACE)
    top = max(float(np.mean(c, axis=0).max()) for c in curves.values()) * 1.1
    for x, sit in zip(ax, SITS):
        style_axes(x)
        for k in (1, 2, 3, 4):
            x.axvline(k * b, color="#b4b2a8", lw=0.8, ls=(0, (2, 2)))
            x.text(k * b, top, f"{k}×", ha="center", va="bottom", fontsize=7, color=MUTED)
        for meth, tr, design in METHODS:
            if (sit, meth) in curves:
                lab = "A: camera alone" if tr == "A" else f"{tr}, {'split' if design == 'split' else 'input offload'}"
                x.plot(GRID, np.mean(curves[sit, meth], axis=0), color=TRANSPORT[tr], lw=2, ls=STYLE[design], label=lab)
        x.set_xlim(0, GRID[-1]); x.set_ylim(0, top)
        x.set_title(sit, fontsize=10, color="#1a1a19", pad=14)
        x.set_xlabel("T_max (s)")
    ax[0].set_ylabel("tasks with ζ ≤ T_max per s")
    h, l = ax[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=4, frameon=False, fontsize=8)
    fig.text(0.01, 0.99, f"Base latency (one camera inference) {b * 1000:.0f} ms; dashed: 1×–4× base. 3 runs per line.",
             va="top", fontsize=8, color=MUTED)
    fig.tight_layout(rect=(0, 0.12, 1, 0.95))
    fig.savefig(out / "fig_throughput.png", dpi=170, facecolor=SURFACE)

    # fig_mission
    fig, ax = plt.subplots(2, 2, figsize=(9.2, 5.6), facecolor=SURFACE, sharex=True)
    names = [f"{'A' if tr == 'A' else tr}\n{'' if design == 'none' else ('split' if design == 'split' else 'input')}"
             for _, tr, design in METHODS]
    for c, sit in enumerate(SITS):
        for r, (data, ylab, fmt) in enumerate(((ev, "evidence ≤ 10 s", "{:.0%}"), (p99, "alert latency p99 (s)", "{:.1f}"))):
            x = ax[r][c]
            style_axes(x)
            for i, (meth, tr, design) in enumerate(METHODS):
                v = data.get((sit, meth))
                if not v:
                    continue
                x.bar(i, np.mean(v), width=0.62, color=TRANSPORT[tr], edgecolor=SURFACE, linewidth=2,
                      hatch="//" if design == "input" else None, alpha=0.95)
                x.plot([i] * len(v), v, "o", ms=4, mfc=SURFACE, mec=INK, mew=0.8)
                x.text(i, np.mean(v), fmt.format(np.mean(v)), ha="center", va="bottom", fontsize=7, color=INK)
            if r == 1:
                x.set_yscale("log")
                x.set_ylim(0.3, 60)
                x.set_xticks(range(len(METHODS)))
                x.set_xticklabels(names, fontsize=7)
            else:
                x.set_ylim(0, 1.12)
                x.set_yticks([0, 0.5, 1]); x.set_yticklabels(["0", "50%", "100%"])
                x.set_title(sit, fontsize=10, color="#1a1a19")
            if c == 0:
                x.set_ylabel(ylab)
    fig.text(0.01, 0.995, "Bars: mean of 3 runs (dots); hatched: input offload (UXGA); plain: split inference or camera alone.",
             va="top", fontsize=8, color=MUTED)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(out / "fig_mission.png", dpi=170, facecolor=SURFACE)

    # fig_chen
    fig, ax = plt.subplots(1, 2, figsize=(9.2, 3.6), sharey=True, facecolor=SURFACE)
    inputs = [x for x in METHODS if x[2] == "input"]
    for x, sit in zip(ax, SITS):
        style_axes(x)
        for i, (meth, tr, _) in enumerate(inputs):
            v = zeta.get((sit, meth))
            if not v:
                continue
            x.boxplot([v], positions=[i], widths=0.5, showfliers=False, patch_artist=True,
                      boxprops=dict(facecolor=TRANSPORT[tr], edgecolor=TRANSPORT[tr], alpha=0.35),
                      medianprops=dict(color=TRANSPORT[tr], lw=2), whiskerprops=dict(color=TRANSPORT[tr]),
                      capprops=dict(color=TRANSPORT[tr]))
            size = np.median([p[0] for p in pred[sit, meth] if p[0]])
            comp = np.median([p[2] for p in pred[sit, meth] if p[2]])
            bws = [p[1] for p in pred[sit, meth] if p[1]]
            x.plot([i - 0.32, i + 0.32], [size / LINK_BPS[sit] + comp] * 2, color="#1a1a19", lw=1.6)
            if bws:
                x.plot([i - 0.32, i + 0.32], [size / np.median(bws) + comp] * 2, color="#1a1a19", lw=1.2, ls=(0, (3, 2)))
        x.set_xticks(range(len(inputs))); x.set_xticklabels([t for _, t, _ in inputs])
        x.set_title(sit, fontsize=10, color="#1a1a19")
    ax[0].set_ylabel("helper task ζ, input offload (s)")
    h = [plt.Line2D([], [], color="#1a1a19", lw=1.6), plt.Line2D([], [], color="#1a1a19", lw=1.2, ls=(0, (3, 2)))]
    fig.legend(h, ["Chen's prediction, R = link rate", "Chen's prediction, R = rate the device measured"],
               loc="lower center", ncol=2, frameon=False, fontsize=8)
    fig.tight_layout(rect=(0, 0.1, 1, 1))
    fig.savefig(out / "fig_chen.png", dpi=170, facecolor=SURFACE)
    print(f"base {b * 1000:.0f} ms; figures in {out}")


if __name__ == "__main__":
    main()
