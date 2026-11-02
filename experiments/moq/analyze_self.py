#!/usr/bin/env python3
"""The mission at the laptop judged without a ground-truth trace: the turntable is periodic, so the
passages of a run must repeat, rotation after rotation, as the same screws in the same order. The
reference (the slots) is taken from the run itself, then the run is judged against it by
analyze_rotations.analyze (missing, duplicate, false, inversions per rotation).

- Period: the shift P (20-60 s) that puts the most passages on another passage P later (within
  TOL_S in time and one cell in y).
- Slots: passages are grouped by (crossing phase mod P, y); a group seen in at least MIN_SHARE of
  the rotations is a screw. A false detection (clutter, a reflection) is at a fixed place in the
  image, not at a fixed phase of the turntable, so it does not repeat with the period; a screw that
  is missed in most rotations is still a slot as long as it is seen in MIN_SHARE of them.
- Speed of each slot at the line: from the passages of its group, the capture time against the travel
  past the line (cap - k P = phase + (x - line) / vx); the median of all slots when a group gives
  no spread. The crossing time of a passage is its capture time minus its travel over that speed,
  computed again with these speeds and the grouping repeated.
No constant depends on the scene: the cell size and the line come from the counter's grid.

usage: analyze_self.py FILE [FILE ...] [--screws N] [--tie-s 0.16] [--json OUT]
  FILE: fused.jsonl or serial.log as analyze_rotations.py; --screws: the number of screws on the
  plate, to report a reference that does not find them all."""
import argparse
import json
from pathlib import Path

import numpy as np

import analyze_rotations as A

LINE = 128
CELL_X, CELL_Y = 256 / 50, 256 / 37.5   # one FOMO cell of the model grid, in result units
TOL_S = 0.5
MIN_SHARE = 0.4
V0 = 15.0                                # first guess of the speed at the line (units/s)


class SelfTemplate:
    """The interface of analyze_rotations.Template, built from the run."""

    def __init__(self, P, phase, y, vx, vy):
        o = np.argsort(phase)
        self.P, self.line, self.cx, self.cy = P, LINE, CELL_X, CELL_Y
        self.phase = np.asarray(phase)[o] - np.min(phase)
        self.y, self.vx, self.vy = np.asarray(y)[o], np.asarray(vx)[o], np.asarray(vy)[o]
        self.n = len(o)


def period(tc, y):
    best = (0, None)
    for P in np.arange(20.0, 60.0, 0.01):
        j = np.searchsorted(tc, tc + P - TOL_S)
        hit = 0
        for i in range(len(tc)):
            k = j[i]
            while k < len(tc) and tc[k] <= tc[i] + P + TOL_S:
                if abs(y[k] - y[i]) <= CELL_Y:
                    hit += 1
                    break
                k += 1
        if hit > best[0]:
            best = (hit, P)
    return best[1]


def group(tc, y, P):
    """greedy grouping on (phase, y), the phase circular; -> list of index arrays."""
    ph = tc % P
    left = list(np.argsort(ph))
    groups = []
    while left:
        i = left.pop(0)
        g = [i]
        for k in list(left):
            d = abs(ph[k] - ph[i])
            if min(d, P - d) <= TOL_S and abs(y[k] - y[i]) <= CELL_Y:
                g.append(k)
                left.remove(k)
        groups.append(np.array(g))
    return groups


def reference(rows, span):
    cap = np.array([r["cap"] for r in rows])
    x = np.array([r["x"] for r in rows], float)
    y = np.array([r["y"] for r in rows], float)
    past = x - LINE
    vx = np.full(len(rows), V0)
    P = None
    for _ in range(2):
        tc = cap - past / vx
        o = np.argsort(tc)
        P = period(tc[o], y[o]) if P is None else P
        if P is None:
            return None
        rotations = (span[1] - span[0]) / P
        slots = []
        for g in group(tc, y, P):
            k = np.floor((cap[g] - cap[g].min()) / P + 0.5)
            if len(set(k)) < max(2, MIN_SHARE * rotations):
                continue
            t = cap[g] - k * P
            if np.ptp(past[g]) >= CELL_X:
                s = np.polyfit(past[g], t, 1)[0]          # = 1 / vx
                v = 1 / s if s > 0 else np.nan
                w = np.polyfit(past[g], y[g], 1)[0] * v if s > 0 else np.nan   # vy = dy/dx * vx
            else:
                v = w = np.nan
            slots.append([g, v, w])
        if not slots:
            return None
        good = [s[1] for s in slots if np.isfinite(s[1]) and 2 < s[1] < 60]
        vmed = float(np.median(good)) if good else V0
        for s in slots:
            if not (np.isfinite(s[1]) and 2 < s[1] < 60):
                s[1], s[2] = vmed, 0.0
            vx[s[0]] = s[1]
        vx[~np.isin(np.arange(len(rows)), np.concatenate([s[0] for s in slots]))] = vmed
    tc = cap - past / vx
    phase = [float(np.median(tc[g] % P)) for g, _, _ in slots]
    # the phases of a group near the wrap: take the circular median
    for n, (g, _, _) in enumerate(slots):
        ph = tc[g] % P
        if np.ptp(ph) > P / 2:
            phase[n] = float(np.median((ph + P / 2) % P) - P / 2) % P
    yl = [float(np.median(y[g] - w * past[g] / v)) for g, v, w in slots]
    return SelfTemplate(P, phase, yl, [s[1] for s in slots], [s[2] for s in slots])


def judge(path, tie_s):
    rows, span = A.load(path)
    if len(rows) < 10:
        return None, None
    tp = reference(rows, span)
    if tp is None:
        return None, None
    return tp, A.analyze(rows, span, tp, tie_s)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--screws", type=int)
    ap.add_argument("--tie-s", type=float, default=0.16)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    out = []
    for f in a.files:
        tp, r = judge(f, a.tie_s)
        if not r:
            print(f"{f}: no reference")
            continue
        r = dict(file=str(f), slots=tp.n, **{k: v for k, v in r.items() if k != "rotations"}, rotations=r["rotations"])
        out.append(r)
        warn = f"  (reference finds {tp.n} of {a.screws} screws)" if a.screws and tp.n != a.screws else ""
        print(f"{f}: period {r['period_s']} s, {tp.n} slots, complete {r['complete_rotations']}/{r['n']}, recall "
              f"{r['slot_recall'] or 0:.3f}, missing {r['missing']} duplicate {r['duplicate']} false {r['false']} "
              f"inversions {r['inversions']}{warn}")
    if a.json:
        a.json.write_text(json.dumps(out, indent=1, default=float) + "\n")


if __name__ == "__main__":
    main()
