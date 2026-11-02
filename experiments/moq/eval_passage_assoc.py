#!/usr/bin/env python3
"""Passage counter ASSOC=0 (screw_mission/passage.py, the device) against ASSOC=1 (passage_assoc.py)
on a trace run, with the ground truth of replay_workers.py.

Sample sets: the device's own inference results (MOQ_TRACK detections at cap_ms), Latest-frame
replays with K = 1, 2, 3 workers over the host detections of the trace frames, and every trace frame.
Scores per half of the scored rotations: tuning was done on the first half of trace-s2-r1 only.

usage: eval_passage_assoc.py RUN_DIR [--seeds 20] [--out JSON]"""
import argparse
import json
from pathlib import Path

import numpy as np

import passage_assoc
import replay_workers as R


def load(run):
    frames = R.load_trace(run)
    cap = dict(frames)
    D = R.Detector()
    det = {s: D(run / "trace" / "frames" / f"{s:07d}.jpg") for s, _ in frames}
    t0, t1 = frames[0][1], frames[-1][1]
    crossings, tracks, flow = R.track_truth(frames, det)
    truth, period, slots, ref = R.periodic_truth(crossings, t0, t1, R.period_estimate([c["t"] for c in crossings]))
    burst = (min(sl["phi"] for sl in slots), max(sl["phi"] for sl in slots))
    rots = sorted({c["rotation"] for c in truth})
    full = [k for k in rots if ref + k * period + burst[0] - 3 >= t0 and ref + k * period + burst[1] + 3 <= t1]
    device = []
    for e in R.log_events(run, "MOQ_TRACK"):
        device.append((e["cap_ms"] / 1e3, e["seq"], [tuple(p) for p in e["pts"]]))
    device.sort()
    dur = np.array([e["duration_us"] / 1e6 for e in R.log_events(run, "AI_PROFILE")])
    st = {e["seq"]: e["start_us"] / 1e6 for e in R.log_events(run, "AI_START")}
    durs = {e["seq"]: e["duration_us"] / 1e6 for e in R.log_events(run, "AI_PROFILE")}
    order = sorted(st, key=st.get)
    over = float(np.median([st[b] - st[a] - durs[a] for a, b in zip(order, order[1:]) if a in durs]))
    passages = R.log_events(run, "MOQ_PASSAGE")
    return dict(frames=frames, cap=cap, det=det, truth=truth, period=period, slots=slots, ref=ref, burst=burst,
                full=full, device=device, durations=dur, overhead=over, passages=passages, t0=t0, t1=t1,
                tracked=len(crossings))


def counter(kind):
    if kind == 0:
        return R.passage.Counter(**R.COUNTER)
    return passage_assoc.Counter(**R.COUNTER)


def feed(kind, samples):
    """samples: [(t_s, seq, pts)] in capture order -> events [(t, x, y)]."""
    c = counter(kind)
    ev = []
    for t, s, pts in samples:
        out = c.update(pts) if kind == 0 else c.update(pts, int(round(t * 1000)))
        for _, p in out:
            ev.append((t, p[0], p[1]))
    return ev


def match_events_optimal(events, crossings, before=0.5, after=3.0, ygate=14):
    """As replay_workers.match_events (same window and cost), but the one-to-one pairing with the
    most matches, then the least cost (assignment), instead of cheapest pair first: in a dense
    burst the greedy pairing can leave a true event and a true crossing unpaired."""
    from scipy.optimize import linear_sum_assignment
    if not events or not crossings:
        return {}
    big = 1e6
    c = np.full((len(events), len(crossings)), big)
    for ei, (te, _, ye) in enumerate(events):
        for ci, x in enumerate(crossings):
            dt = te - x["t"]
            if -before <= dt <= after and abs(ye - x["y"]) <= ygate:
                c[ei, ci] = abs(ye - x["y"]) / ygate + max(dt, 0) / after
    r, k = linear_sum_assignment(c)
    return {int(ci): int(ei) for ei, ci in zip(r, k) if c[ei, ci] < big}


MATCH = R.match_events


def score(D, ev, rotations):
    """recall and false events over the given rotations (events attributed by time window)."""
    truth = D["truth"]
    idx = [i for i, c in enumerate(truth) if c["rotation"] in rotations]
    lo = lambda k: D["ref"] + k * D["period"] + D["burst"][0] - 3
    hi = lambda k: D["ref"] + k * D["period"] + D["burst"][1] + 3
    win = [e for e in ev if any(lo(k) <= e[0] <= hi(k) for k in rotations)]
    m = MATCH(win, truth)
    hit = sum(1 for i in idx if i in m)
    return dict(hit=hit, n=len(idx), recall=hit / len(idx) if idx else None, fp=len(win) - len(m))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=Path)
    ap.add_argument("--seeds", type=int, default=20)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--matching", choices=("greedy", "optimal"), default="greedy",
                    help="event-to-crossing pairing of the score")
    a = ap.parse_args()
    global MATCH
    MATCH = R.match_events if a.matching == "greedy" else match_events_optimal
    D = load(a.run)
    full = D["full"]
    h = len(full) // 2   # the first half of the rotations of the run (rotation 0 is not scored)
    halves = dict(first=full[:h], second=full[h:], all=full)
    det, cap = D["det"], D["cap"]
    sets = {"device": [D["device"]], "all_frames": [[(t, s, det[s]) for s, t in D["frames"]]]}
    service = lambda rng: float(rng.choice(D["durations"])) + D["overhead"]
    times = [t for _, t in D["frames"]]
    for k in (1, 2, 3):
        runs = []
        for seed in range(a.seeds):
            rng = np.random.default_rng(seed)
            chosen = R.latest_workers(D["frames"], k, service, rng, start=times[0] + rng.uniform(0, 1.2))
            runs.append(sorted((cap[s], s, det[s]) for s in chosen))
        sets[f"latest_K{k}"] = runs
    table = {}
    for name, runs in sets.items():
        for kind in (0, 1):
            res = {hn: [score(D, feed(kind, smp), rots) for smp in runs] for hn, rots in halves.items()}
            table[f"{name}/assoc{kind}"] = {hn: dict(recall=float(np.mean([r["recall"] for r in rs])),
                                                     recall_sd=float(np.std([r["recall"] for r in rs])),
                                                     fp=float(np.mean([r["fp"] for r in rs])), n=rs[0]["n"])
                                            for hn, rs in res.items()}
    dev_events = feed(0, D["device"])
    log = [(e["x"], e["y"]) for e in D["passages"]]
    summary = dict(run=str(a.run), matching=a.matching, rotations=dict(first=halves["first"], second=halves["second"]),
                   truth_crossings_scored=sum(1 for c in D["truth"] if c["rotation"] in full),
                   tracked_crossings=D["tracked"], slots=len(D["slots"]), period_s=D["period"],
                   truth_per_rotation={k: sum(1 for c in D["truth"] if c["rotation"] == k) for k in full},
                   assoc0_reproduces_device_log=[(x, y) for _, x, y in dev_events] == log,
                   table=table)
    print(json.dumps(summary, indent=1))
    if a.out:
        a.out.write_text(json.dumps(summary, indent=1) + "\n")


if __name__ == "__main__":
    main()
