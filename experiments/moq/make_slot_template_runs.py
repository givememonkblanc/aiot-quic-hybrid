#!/usr/bin/env python3
"""Slot template of a turntable scene from the passages of many runs, for analyze_rotations.py.

Why (10/8, scene 3): the frame traces of the scene lost most of their frames (make_slot_template.py
cannot track them), and the labelled frames are too sparse to be moved to the line with a velocity
field without spreading one screw over several slots (make_slot_template_labels.py). A passage is
reported at the line (x 130-140), so its crossing time needs almost no velocity; a run of 213 s
holds six rotations, and runs of the same scene pooled hold dozens.

- Crossing time tc = capture - (x - line) / vx, vx and vy at the line from the velocity field of the
  labelled frames (--labels, make_slot_template_labels.velocity; not from the model).
- Each run alone: its period (the shift of 1 to 4 periods that puts the most crossings on another
  one), its crossings grouped by (phase, y); a group seen in at least MIN_SHARE of its rotations is a
  screw place, its multiplicity the median number of passages per rotation in which it is seen
  (touching screws pass at one place: the count head reports each). Phases from the first place
  after the longest gap of the rotation.
- Consensus: the places of all runs, each run aligned on the run with the most places by the phase
  shift that matches the most, grouped again; a place of at least half of the runs is a slot, at its
  median phase, y and multiplicity. (Pooling the crossings of all runs first spreads the groups:
  the period of each run is known to 0.02 s only.)
- Checks: the number of screws against --screws (the plate, counted by hand), and with --loo each run
  judged on the template of the other runs (analyze_rotations.analyze).
The template is made by the model of the runs: a screw it never finds is no slot, which the count
against --screws shows.

usage: make_slot_template_runs.py RUN... --labels L.jsonl... --out JSON [--screws 19] [--loo]"""
import argparse
import json
import tempfile
from pathlib import Path

import numpy as np

import analyze_rotations as A
import make_slot_template_labels as L

LINE = 128
CX, CY = L.CX, L.CY
TOL_S = 0.3
MIN_SHARE = 0.4


def period(tc, yl):
    o = np.argsort(tc)
    tc, yl = tc[o], yl[o]

    def score(P, tol):
        hit = 0
        for m in (1, 2, 3, 4):
            j = np.searchsorted(tc, tc + m * P - tol)
            k = np.searchsorted(tc, tc + m * P + tol)
            hit += sum(np.any(np.abs(yl[j[i]:k[i]] - yl[i]) <= CY) for i in range(len(tc)) if k[i] > j[i])
        return hit
    P = max(np.arange(33.0, 38.0, 0.02), key=lambda P: score(P, TOL_S))
    return max(np.arange(P - 0.06, P + 0.06, 0.002), key=lambda P: score(P, TOL_S / 2))


def crossings(rows, F):
    x = np.array([r["x"] for r in rows], float)
    y = np.array([r["y"] for r in rows], float)
    cap = np.array([r["cap"] for r in rows])
    vx = F[0] @ np.array([np.full_like(y, LINE), y, np.ones_like(y)])
    vy = F[1] @ np.array([np.full_like(y, LINE), y, np.ones_like(y)])
    return cap - (x - LINE) / vx, y - vy * (x - LINE) / vx


def shift_on(ref, ph, yl, P):
    best = (-1, 0.0)
    for sh in np.arange(0, P, 0.02):
        q = (ph + sh) % P
        d = np.abs(((q[:, None] - ref[0][None]) + P / 2) % P - P / 2)
        m = int(((d <= TOL_S) & (np.abs(yl[:, None] - ref[1][None]) <= CY)).any(1).sum())
        if m > best[0]:
            best = (m, sh)
    return best[1]


def build_one(runs, F, screws):
    data = []
    for run in runs:
        rows, span = A.load(run / "serial.log")
        tc, yl = crossings(rows, F)
        data.append((run, tc, yl, period(tc, yl), span))
    P = float(np.median([d[3] for d in data]))
    phase = [((tc - tc.min()) % Pi) * (P / Pi) for _, tc, _, Pi, _ in data]   # each run on the common period
    ref_i = int(np.argmax([len(d[1]) for d in data]))
    ref = (phase[ref_i], data[ref_i][2])
    ph, yy, rot = [], [], []
    rotations = 0
    for i, (run, tc, yl, Pi, span) in enumerate(data):
        p = phase[i]
        sh = 0.0 if i == ref_i else shift_on(ref, p, yl, P)
        r = np.floor((tc - tc.min()) / Pi).astype(int)
        ph.append((p + sh) % P)
        yy.append(yl)
        rot.append(r + rotations)
        rotations += int(r.max()) + 1
    ph, yy, rot = np.concatenate(ph), np.concatenate(yy), np.concatenate(rot)
    left = set(range(len(ph)))
    slots = []
    for i in np.argsort(ph):
        if i not in left:
            continue
        def near(c, cy):
            return [k for k in left if min(abs(ph[k] - c), P - abs(ph[k] - c)) <= TOL_S and abs(yy[k] - cy) <= CY]

        def centre(g):
            return np.angle(np.mean(np.exp(2j * np.pi * ph[g] / P))) * P / (2 * np.pi) % P, float(np.median(yy[g]))
        c, cy = centre(near(ph[i], yy[i]))                  # the centre of the group around i, then the group
        g = near(c, cy)
        c, cy = centre(g)
        left -= set(g)
        seen = np.bincount(rot[g])
        seen = seen[seen > 0]
        if len(seen) < MIN_SHARE * rotations:
            continue
        g = np.array(g)
        slots.append(dict(phi=float(c), y=cy, seen=len(seen), mult=int(np.median(seen)),
                          phase_sd_s=float(np.std(((ph[g] - c + P / 2) % P) - P / 2)), y_sd=float(np.std(yy[g]))))
    slots.sort(key=lambda s: s["phi"])
    gaps = [(slots[(k + 1) % len(slots)]["phi"] - slots[k]["phi"]) % P for k in range(len(slots))]
    start = (int(np.argmax(gaps)) + 1) % len(slots)
    slots = slots[start:] + slots[:start]
    p0 = slots[0]["phi"]
    out = []
    for s in slots:
        for _ in range(s["mult"]):
            out.append(dict(slot=len(out), phase_s=round((s["phi"] - p0) % P, 3), y=round(s["y"], 1),
                            vx=round(float(F[0] @ (LINE, s["y"], 1)), 1), vy=round(float(F[1] @ (LINE, s["y"], 1)), 1),
                            seen=s["seen"], multiplicity=s["mult"], phase_sd_s=round(s["phase_sd_s"], 3), y_sd=round(s["y_sd"], 1)))
    return dict(source=f"passages of {len(runs)} runs ({', '.join(r.name for r in runs)}), crossings grouped by "
                       f"rotation phase (make_slot_template_runs.py); velocity from labelled frames",
                screws=len(out), screws_by_hand=screws, places=len(slots), period_s=round(P, 4), line=LINE, axis="x",
                cell=dict(x=round(CX, 2), y=round(CY, 2)), rotations=rotations,
                periods={d[0].name: round(float(d[3]), 3) for d in data}, slots=out)


def build(runs, F, screws):
    one = [build_one([r], F, screws) for r in runs]
    if len(one) == 1:
        return one[0]
    P = float(np.median([t["period_s"] for t in one]))

    def places(t):
        seen = {}
        for q in t["slots"]:
            seen.setdefault((q["phase_s"], q["y"]), q)
        return [(k[0] * P / t["period_s"], k[1], q["multiplicity"]) for k, q in seen.items()]
    ref_i = int(np.argmax([t["places"] for t in one]))
    ref = np.array(places(one[ref_i]))
    pts = []
    for i, t in enumerate(one):
        p = np.array(places(t))
        sh = 0.0 if i == ref_i else shift_on((ref[:, 0], ref[:, 1]), p[:, 0], p[:, 1], P)
        sh = sh if sh <= P / 2 else sh - P
        for ph, y, m in p:
            pts.append(((ph + sh) % P, y, m, i))
    pts = np.array(pts)
    left = set(range(len(pts)))
    slots = []
    for i in np.argsort(pts[:, 0]):
        if i not in left:
            continue
        def near(c, cy):
            return [k for k in left if min(abs(pts[k, 0] - c), P - abs(pts[k, 0] - c)) <= TOL_S and abs(pts[k, 1] - cy) <= CY]
        g = near(pts[i, 0], pts[i, 1])
        c = np.angle(np.mean(np.exp(2j * np.pi * pts[g, 0] / P))) * P / (2 * np.pi) % P
        g = near(c, float(np.median(pts[g, 1])))
        left -= set(g)
        runs_seen = len(set(pts[g, 3].astype(int)))
        if runs_seen < len(one) / 2:
            continue
        c = np.angle(np.mean(np.exp(2j * np.pi * pts[g, 0] / P))) * P / (2 * np.pi) % P
        slots.append(dict(phi=float(c), y=float(np.median(pts[g, 1])), runs=runs_seen, mult=int(np.median(pts[g, 2])),
                          phase_sd_s=float(np.std(((pts[g, 0] - c + P / 2) % P) - P / 2)), y_sd=float(np.std(pts[g, 1]))))
    slots.sort(key=lambda s: s["phi"])
    gaps = [(slots[(k + 1) % len(slots)]["phi"] - slots[k]["phi"]) % P for k in range(len(slots))]
    start = (int(np.argmax(gaps)) + 1) % len(slots)
    slots = slots[start:] + slots[:start]
    p0 = slots[0]["phi"]
    out = []
    for s in slots:
        for _ in range(s["mult"]):
            out.append(dict(slot=len(out), phase_s=round((s["phi"] - p0) % P, 3), y=round(s["y"], 1),
                            vx=round(float(F[0] @ (LINE, s["y"], 1)), 1), vy=round(float(F[1] @ (LINE, s["y"], 1)), 1),
                            runs=s["runs"], multiplicity=s["mult"], phase_sd_s=round(s["phase_sd_s"], 3), y_sd=round(s["y_sd"], 1)))
    return dict(source=f"consensus of the places of {len(runs)} runs ({', '.join(r.name for r in runs)}), each grouped by "
                       f"rotation phase (make_slot_template_runs.py); velocity from labelled frames",
                screws=len(out), screws_by_hand=screws, places=len(slots), period_s=round(P, 4), line=LINE, axis="x",
                cell=dict(x=round(CX, 2), y=round(CY, 2)), rotations=sum(t["rotations"] for t in one),
                per_run={r.name: dict(screws=t["screws"], period_s=t["period_s"]) for r, t in zip(runs, one)}, slots=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--labels", nargs="+", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--screws", type=int, default=19)
    ap.add_argument("--loo", action="store_true")
    a = ap.parse_args()
    fr = L.frames_of(a.labels)
    F, _ = L.velocity([f for s in fr.values() for f in s] if len(fr) == 1 else max(fr.values(), key=len))
    t = build(a.runs, F, a.screws)
    a.out.write_text(json.dumps(t, indent=1) + "\n")
    print(f"{len(a.runs)} runs, {t['rotations']} rotations, period {t['period_s']} s: {t['places']} places, "
          f"{t['screws']} screws (by hand {a.screws}); runs per place {[s.get('runs') for s in t['slots']]}")
    if a.loo:
        for i, run in enumerate(a.runs):
            others = a.runs[:i] + a.runs[i + 1:]
            ti = build(others, F, a.screws)
            with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
                f.write(json.dumps(ti))
            rows, span = A.load(run / "serial.log")
            r = A.analyze(rows, span, A.Template(f.name), 0.16)
            print(f"  {run.name} on the other runs ({ti['screws']} screws): " + (
                f"complete {r['complete_rotations']}/{r['n']}, recall {r['slot_recall']:.3f}, missing {r['missing']}, "
                f"duplicate {r['duplicate']}, false {r['false']}, inversions {r['inversions']}" if r else "no fit"))


if __name__ == "__main__":
    main()
