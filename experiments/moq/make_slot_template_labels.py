#!/usr/bin/env python3
"""Slot template of a turntable scene from labelled frames (label_turntable.py), for analyze_rotations.py.

make_slot_template.py tracks the detections of the device model in every frame of a trace; a trace
that lost most of its frames on the way (scene 3: 236 of 1327) cannot be tracked, and a template made
by the model would judge that model by itself. Here the reference comes from the labels of the
full-resolution frames (dark regions of the plate, not the model) and from the turntable being
periodic, so the frames need not be consecutive:

- Velocity field: screws of two frames less than MAX_DT apart, matched nearest to nearest, give
  displacements; a rigid rotation seen by the camera is a velocity linear in the position,
  v = A p + b, fitted by least squares (the speed along the counting band changes from 14 to 6
  units/s, so one straight-line speed puts the same screw in two or three slots).
- Each labelled screw within NEAR of the line is moved along that field to the line: its crossing
  time and its y there.
- Period: the shift P that puts the most crossings on another crossing P later (within TOL_S and
  one cell in y).
- Slots: crossings grouped by (tc mod P, y at line); a group seen in at least MIN_SHARE of the
  rotations of the session is a screw.
Sessions are built one by one and must agree (number of slots, phases and y up to a shift of the
phase); the template is the first session, or --session.

usage: make_slot_template_labels.py LABELS.jsonl... --out JSON [--session S] [--screws 19]"""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

import numpy as np

LINE = 128
CX, CY = 256 * 0.4 / 20, 256 * 0.4 / 15
Y0, Y1 = 256 * 0.3, 256 * 0.7
NEAR = 40
MAX_DT = 1.5
TOL_S = 0.35
MIN_SHARE = 0.5


def frames_of(labels):
    """{session: [(t, [(x, y)...])]} with the capture time from the trace index."""
    out = defaultdict(list)
    times = {}
    for f in labels:
        for line in open(f):
            r = json.loads(line)
            if r.get("uncertain"):
                continue
            fr = Path(r["frame"])
            idx = fr.parent.parent / "index.csv"
            if idx not in times:
                times[idx] = {int(row[0]): float(row[3]) for row in csv.reader(open(idx))}
            t = times[idx].get(int(fr.stem))
            if t is None:
                continue
            pts = [(o["cx"] * 256, o["cy"] * 256) for o in r["objects"]]
            out[r["session"]].append((t, pts))
    return {s: sorted(v) for s, v in out.items()}


def velocity(frames):
    d = []
    for (t0, a), (t1, b) in zip(frames, frames[1:]):
        dt = t1 - t0
        if not 0 < dt <= MAX_DT or not a or not b:
            continue
        B = np.array(b)
        for x, y in a:
            if abs(x - LINE) > NEAR or not Y0 - CY <= y <= Y1 + CY:
                continue
            k = np.argmin(np.hypot(B[:, 0] - x, B[:, 1] - y))
            dx, dy = B[k, 0] - x, B[k, 1] - y
            if 0 < dx < 40 * dt and abs(dy) < 20 * dt:      # moving with the plate, one screw
                d.append((x + dx / 2, y + dy / 2, dx / dt, dy / dt))
    d = np.array(d)
    X = np.c_[d[:, 0], d[:, 1], np.ones(len(d))]
    keep = np.ones(len(d), bool)
    for _ in range(3):                                   # drop wrong matches
        cx = np.linalg.lstsq(X[keep], d[keep, 2], rcond=None)[0]
        cy = np.linalg.lstsq(X[keep], d[keep, 3], rcond=None)[0]
        r = np.hypot(X @ cx - d[:, 2], X @ cy - d[:, 3])
        keep = r <= 3 * np.median(r[keep])
    return np.array([cx, cy]), int(keep.sum())


def to_line(F, x, y):
    """moved along v = F [x, y, 1] to x = LINE: (time to get there, y there, vx, vy there)."""
    t, h = 0.0, 0.02
    sgn = 1.0 if x < LINE else -1.0
    for _ in range(4000):
        vx, vy = F @ (x, y, 1.0)
        if (LINE - x) * sgn <= 0 or vx <= 0.5:
            break
        x, y, t = x + sgn * h * vx, y + sgn * h * vy, t + sgn * h
    vx, vy = F @ (x, y, 1.0)
    return t, y, vx, vy


def crossings(frames, F):
    c = []
    for t, pts in frames:
        for x, y in pts:
            if abs(x - LINE) <= NEAR and Y0 - 2 * CY <= y <= Y1 + 2 * CY:
                dt, yl, _, _ = to_line(F, x, y)
                if Y0 <= yl <= Y1:
                    c.append((t + dt, yl, t, x, y))
    return np.array(c)


def period(tc, yl):
    """shifts of 1 to 4 periods at once: an error of the period grows with the shift."""
    o = np.argsort(tc)
    tc, yl = tc[o], yl[o]

    def score(P, tol):
        hit = 0
        for m in (1, 2, 3, 4):
            j = np.searchsorted(tc, tc + m * P - tol)
            k = np.searchsorted(tc, tc + m * P + tol)
            hit += sum(np.any(np.abs(yl[j[i]:k[i]] - yl[i]) <= CY) for i in range(len(tc)) if k[i] > j[i])
        return hit
    P = max(np.arange(30.0, 40.0, 0.02), key=lambda P: score(P, TOL_S))
    return max(np.arange(P - 0.1, P + 0.1, 0.002), key=lambda P: score(P, TOL_S / 2))


def slots(c, P):
    tc, yl = c[:, 0], c[:, 1]
    ph = tc % P
    rot = np.floor((tc - tc.min()) / P).astype(int)
    rotations = rot.max() + 1
    left = set(range(len(c)))
    out = []
    for i in np.argsort(ph):
        if i not in left:
            continue
        d = np.abs(ph - ph[i])
        g = [k for k in left if min(d[k], P - d[k]) <= TOL_S and abs(yl[k] - yl[i]) <= CY]
        # grow once around the group's centre
        cph = np.angle(np.mean(np.exp(2j * np.pi * ph[g] / P))) * P / (2 * np.pi) % P
        cy = np.median(yl[g])
        d = np.abs(ph - cph)
        g = [k for k in left if min(d[k], P - d[k]) <= TOL_S and abs(yl[k] - cy) <= CY]
        left -= set(g)
        seen = len(set(rot[g]))
        if seen < MIN_SHARE * rotations:
            continue
        g = np.array(g)
        cph = np.angle(np.mean(np.exp(2j * np.pi * ph[g] / P))) * P / (2 * np.pi) % P
        out.append(dict(phi=float(cph), y=float(np.median(yl[g])), seen=seen, n=len(g),
                        phase_sd_s=float(np.std(((ph[g] - cph + P / 2) % P) - P / 2)), y_sd=float(np.std(yl[g])),
                        members=g))
    return out, rotations


def build(frames):
    F, nv = velocity(frames)
    c = crossings(frames, F)
    P = period(c[:, 0], c[:, 1])
    sl, rotations = slots(c, P)
    sl.sort(key=lambda s: s["phi"])
    gaps = [(sl[(k + 1) % len(sl)]["phi"] - sl[k]["phi"]) % P for k in range(len(sl))]
    start = (int(np.argmax(gaps)) + 1) % len(sl)
    sl = sl[start:] + sl[:start]
    p0 = sl[0]["phi"]
    out = []
    for rank, s in enumerate(sl):
        out.append(dict(slot=rank, phase_s=round((s["phi"] - p0) % P, 3), y=round(s["y"], 1),
                        vx=round(float(F[0] @ (LINE, s["y"], 1)), 1), vy=round(float(F[1] @ (LINE, s["y"], 1)), 1), seen=s["seen"],
                        phase_sd_s=round(s["phase_sd_s"], 3), y_sd=round(s["y_sd"], 1)))
    span = (frames[0][0], frames[-1][0])
    return dict(period_s=round(float(P), 4), rotations=int(rotations), frames=len(frames), span_s=round(span[1] - span[0], 1),
                velocity_pairs=nv, crossings=len(c), slots=out)


def agreement(a, b):
    """best circular alignment of b's phases on a's; -> (shift, matched, phase err, y err)."""
    pa, ya = np.array([s["phase_s"] for s in a["slots"]]), np.array([s["y"] for s in a["slots"]])
    pb, yb = np.array([s["phase_s"] for s in b["slots"]]), np.array([s["y"] for s in b["slots"]])
    P = a["period_s"]
    best = None
    for sh in np.arange(0, P, 0.02):
        q = (pb + sh) % P
        dp = np.abs(((q[None] - pa[:, None]) + P / 2) % P - P / 2)
        dy = np.abs(yb[None] - ya[:, None])
        ok = (dp <= TOL_S) & (dy <= CY)
        m = int(ok.any(1).sum())
        if best is None or m > best[1]:
            j = np.where(ok, dp, 9).argmin(1)
            e = [(dp[i, j[i]], dy[i, j[i]]) for i in range(len(pa)) if ok[i].any()]
            best = (round(float(sh), 2), m, round(float(np.median([x[0] for x in e])), 3) if e else None,
                    round(float(np.median([x[1] for x in e])), 1) if e else None)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("labels", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--session")
    ap.add_argument("--screws", type=int, default=19)
    a = ap.parse_args()
    fr = frames_of(a.labels)
    built = {}
    for s, f in fr.items():
        built[s] = b = build(f)
        warn = "" if len(b["slots"]) == a.screws else f"  (expected {a.screws})"
        print(f"{s}: {b['frames']} frames over {b['span_s']} s, period {b['period_s']} s, {b['rotations']} rotations, "
              f"{b['crossings']} labelled screws near the line, {len(b['slots'])} slots{warn}; "
              f"seen {[q['seen'] for q in b['slots']]}")
    names = list(built)
    ref = a.session or names[0]
    for s in names:
        if s != ref:
            sh, m, ep, ey = agreement(built[ref], built[s])
            print(f"  {s} on {ref}: {m} of {len(built[ref]['slots'])} slots agree (phase shift {sh} s, "
                  f"median phase error {ep} s, y error {ey})")
    b = built[ref]
    t = dict(source=f"{ref}: label_turntable.py labels of the trace frames (not a model), crossings grouped by "
                    f"rotation phase (make_slot_template_labels.py)",
             screws=len(b["slots"]), period_s=b["period_s"], line=LINE, axis="x",
             cell=dict(x=round(CX, 2), y=round(CY, 2)),
             trace=dict(frames=b["frames"], span_s=b["span_s"], rotations=b["rotations"], labelled_crossings=b["crossings"]),
             sessions={s: dict(period_s=v["period_s"], slots=len(v["slots"])) for s, v in built.items()},
             slots=b["slots"])
    a.out.write_text(json.dumps(t, indent=1) + "\n")


if __name__ == "__main__":
    main()
