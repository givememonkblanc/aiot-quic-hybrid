#!/usr/bin/env python3
"""The mission at the laptop: in every rotation of the turntable, each of the 19 screws must arrive
once, in the order in which the screws passed the counting line.

A run is judged against a slot template (by default the newest results/trace-*/slot_template.json,
written by make_slot_template.py from the all-frame ground truth of a trace of the same scene): for each screw its crossing phase within the
rotation, its y at the line and its velocity (vx, vy) at the line, and the period.

- Crossing time of a passage, for slot j: cap - (x - line) / vx_j (the travel past the line at the
  speed of that screw); its y at the line: y - vy_j (x - line) / vx_j.
- Run alignment: the phase offset and the period of the run (it may differ slightly from the
  template's) are found by maximizing the passages that fall on a slot, then refined by least
  squares on the matched passages.
- Assignment: passages to slot instances (rotation k, slot j) by optimal matching (most matches,
  then least cost). Tolerance: one detection cell along each axis (cell x / vx_j in time, cell y in
  y). A passage is a quantized FOMO cell (+-1/2 cell), and so is the template position: their
  difference is at most one cell.
- A passage outside the tolerance of every slot is false; one inside the tolerance of a slot
  instance already matched is a duplicate.
- Order: the arrival order at the laptop (t_out of fused.jsonl; the time of the line in serial.log;
  ties in the order of the file) against the physical order of the slots. Inversions = pairs of
  matched passages of a rotation whose arrival order differs from their slot order; those between
  passages with the same arrival time (one result) are also given apart. With --tie-s D, a pair of
  slots whose template phases differ by less than D (one frame interval of the ground-truth trace:
  the truth itself does not order them) is concurrent: either arrival order is right; such pairs are
  given apart as inversions_concurrent and do not count as inversions.
A rotation is complete when all slots arrive exactly once (no missing, no duplicate, no false) and
there is no inversion. Only rotations wholly inside the observed capture span are judged.

usage: analyze_rotations.py FILE [FILE ...] [--template JSON] [--tie-s 0.16] [--json OUT]
  FILE: fused.jsonl (lines with t_out, cap_ms, x, y) or serial.log ([MOQ_PASSAGE] seq mapped to the
  capture time by [MOQ_TRACK] cap_ms)"""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.optimize import linear_sum_assignment

RESULTS = Path(__file__).resolve().parent / "results"
TEMPLATE = RESULTS / "trace-s2-r2" / "slot_template.json"


def newest_template():
    """The most recently written results/trace-*/slot_template.json (make_slot_template.py)."""
    found = sorted(RESULTS.glob("trace-*/slot_template.json"), key=lambda p: p.stat().st_mtime)
    return found[-1] if found else TEMPLATE


def load(path):
    """-> rows [dict(arrival, rank, cap, x, y)], capture span (s)."""
    path = Path(path)
    rows = []
    if path.name.endswith(".jsonl"):
        for i, line in enumerate(path.read_text().splitlines()):
            if not line.strip():
                continue
            r = json.loads(line)
            if "cap_ms" in r and "x" in r:
                rows.append(dict(arrival=float(r["t_out"]), rank=i, cap=r["cap_ms"] / 1000, x=r["x"], y=r["y"]))
        caps = [r["cap"] for r in rows]
        return rows, (min(caps), max(caps)) if caps else (0, 0)
    cap_of = {}
    lines = path.read_text(errors="replace").splitlines()
    for line in lines:
        if "[MOQ_TRACK] " in line:
            try:
                r = json.loads(line[line.index("{"):])
            except ValueError:
                continue
            cap_of[r["seq"]] = r["cap_ms"] / 1000
    for i, line in enumerate(lines):
        if "[MOQ_PASSAGE] " in line:
            try:
                r = json.loads(line[line.index("{"):])
            except ValueError:
                continue
            if r["seq"] in cap_of:
                rows.append(dict(arrival=float(line.split()[0]), rank=i, cap=cap_of[r["seq"]], x=r["x"], y=r["y"]))
    caps = list(cap_of.values())
    return rows, (min(caps), max(caps)) if caps else (0, 0)


class Template:
    def __init__(self, path):
        t = json.loads(Path(path).read_text())
        self.P = t["period_s"]
        self.line = t["line"]
        self.cx, self.cy = t["cell"]["x"], t["cell"]["y"]
        s = t["slots"]
        self.phase = np.array([q["phase_s"] for q in s])
        self.y = np.array([q["y"] for q in s])
        self.vx = np.array([q["vx"] for q in s])
        self.vy = np.array([q["vy"] for q in s])
        self.n = len(s)

    def residuals(self, row, T):
        """for each slot j: (crossing time - T[j], y at line - y_j)."""
        past = row["x"] - self.line
        tc = row["cap"] - past / self.vx
        yl = row["y"] - self.vy * past / self.vx
        return tc - T, yl - self.y

    def inside(self, dt, dy):
        """normalized distance (<= 1: within one cell along each axis)."""
        return np.maximum(np.abs(dt) * self.vx / self.cx, np.abs(dy) / self.cy)


def fit(rows, tp):
    """phase offset (time of slot 0 of rotation 0) and period of the run."""
    past = np.array([r["x"] - tp.line for r in rows])
    v = np.mean(tp.vx)
    tc = np.array([r["cap"] for r in rows]) - past / v
    yl = np.array([r["y"] for r in rows]) - np.mean(tp.vy) * past / v
    best = None
    tol = tp.cx / v
    for P in np.arange(tp.P * 0.99, tp.P * 1.01, 0.002):
        ph = tc % P
        d = (ph[:, None] - tp.phase[None, :]) % P              # offsets that put passage i on slot j
        ok = np.abs(yl[:, None] - tp.y[None, :]) <= tp.cy
        cand = d[ok]
        if not len(cand):
            continue
        bins = np.zeros(int(P / 0.02) + 1)
        np.add.at(bins, (cand / 0.02).astype(int), 1)
        w = int(round(tol / 0.02))
        conv = sum(np.roll(bins, k) for k in range(-w, w + 1))
        k = int(np.argmax(conv))
        if best is None or conv[k] > best[0]:
            best = (conv[k], P, k * 0.02)
    _, P, off = best
    return off, P


def assign(rows, tp, off, P, span):
    k0 = int(np.floor((span[0] - off) / P)) - 1
    k1 = int(np.ceil((span[1] - off) / P)) + 1
    inst = [(k, j) for k in range(k0, k1 + 1) for j in range(tp.n)]
    T = np.array([off + k * P + tp.phase[j] for k, j in inst])
    big = 1e6
    C = np.full((len(rows), len(inst)), big)
    for i, r in enumerate(rows):
        near = np.where(np.abs(T - r["cap"]) < 5)[0]
        if not len(near):
            continue
        j = np.array([inst[q][1] for q in near])
        past = r["x"] - tp.line
        tc = r["cap"] - past / tp.vx[j]
        yl = r["y"] - tp.vy[j] * past / tp.vx[j]
        dist = np.maximum(np.abs(tc - T[near]) * tp.vx[j] / tp.cx, np.abs(yl - tp.y[j]) / tp.cy)
        C[i, near] = np.where(dist <= 1, dist, big)
    r_, c_ = linear_sum_assignment(C)
    match = {int(i): int(q) for i, q in zip(r_, c_) if C[i, q] < big}
    return inst, T, C, match


def analyze(rows, span, tp, tie_s=0.0):
    if len(rows) < tp.n:
        return None
    off, P = fit(rows, tp)
    for _ in range(2):   # refine offset and period on the matched passages
        inst, T, C, match = assign(rows, tp, off, P, span)
        if len(match) < 3:
            break
        ks = np.array([inst[q][0] for q in match.values()], float)
        tc = np.array([rows[i]["cap"] - (rows[i]["x"] - tp.line) / tp.vx[inst[q][1]] - tp.phase[inst[q][1]]
                       for i, q in match.items()])
        A = np.c_[np.ones(len(ks)), ks]
        off, P = np.linalg.lstsq(A, tc, rcond=None)[0]
    inst, T, C, match = assign(rows, tp, off, P, span)
    matched_inst = {q: i for i, q in match.items()}
    kind = {}
    for i in range(len(rows)):
        if i in match:
            kind[i] = ("match", inst[match[i]])
        else:
            q = np.where(C[i] < 1e6)[0]
            q = [x for x in q if x in matched_inst]
            if q:
                best = min(q, key=lambda x: C[i, x])
                kind[i] = ("duplicate", inst[best])
            else:
                kind[i] = ("false", None)
    out = []
    ks = sorted({k for k, _ in inst})
    for k in ks:
        first, last = off + k * P + tp.phase.min(), off + k * P + tp.phase.max()
        if first - 1 < span[0] or last + 3 > span[1]:
            continue   # not wholly observed
        mine = [i for i, (kd, q) in kind.items() if q is not None and q[0] == k]
        got = [i for i in mine if kind[i][0] == "match"]
        dup = [i for i in mine if kind[i][0] == "duplicate"]
        lo, hi = first - 2, last + 3
        false = [i for i, (kd, q) in kind.items() if kd == "false" and lo <= rows[i]["cap"] - (rows[i]["x"] - tp.line) / np.mean(tp.vx) <= hi]
        slots = {kind[i][1][1]: i for i in got}
        missing = [j for j in range(tp.n) if j not in slots]
        arr = sorted(got, key=lambda i: (rows[i]["arrival"], rows[i]["rank"]))
        inv = tie = conc = 0
        for a in range(len(arr)):
            for b in range(a + 1, len(arr)):
                ja, jb = kind[arr[a]][1][1], kind[arr[b]][1][1]
                if ja > jb:
                    if abs(tp.phase[ja] - tp.phase[jb]) < tie_s:
                        conc += 1
                        continue
                    inv += 1
                    tie += rows[arr[a]]["arrival"] == rows[arr[b]]["arrival"]
        late = [round(rows[i]["arrival"] - rows[i]["cap"], 2) for i in got]
        out.append(dict(rotation=k, received=len(got) + len(dup) + len(false), slots=len(slots),
                        missing=missing, duplicate=len(dup), false=len(false), inversions=inv,
                        inversions_same_arrival=tie, inversions_concurrent=conc,
                        complete=not missing and not dup and not false and inv == 0))
    return dict(period_s=round(float(P), 3), offset_s=round(float(off), 3), passages=len(rows),
                rotations=out, n=len(out), complete_rotations=sum(o["complete"] for o in out),
                missing=sum(len(o["missing"]) for o in out), duplicate=sum(o["duplicate"] for o in out),
                false=sum(o["false"] for o in out), inversions=sum(o["inversions"] for o in out),
                slot_recall=(sum(o["slots"] for o in out) / (tp.n * len(out))) if out else None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--template", type=Path, help="default: the newest results/trace-*/slot_template.json")
    ap.add_argument("--tie-s", type=float, default=0.0, help="slots closer than this in phase are concurrent")
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    tpath = a.template or newest_template()
    tp = Template(tpath)
    print(f"template: {tpath} ({tp.n} slots, period {tp.P} s)")
    allres = {}
    for f in a.files:
        rows, span = load(f)
        res = analyze(rows, span, tp, a.tie_s)
        allres[f] = res
        if not res:
            print(f"{f}: too few passages ({len(rows)})")
            continue
        print(f"{f}: {res['passages']} passages, period {res['period_s']} s, complete rotations "
              f"{res['complete_rotations']}/{res['n']}, slot recall {res['slot_recall']:.3f}, missing {res['missing']}, "
              f"duplicate {res['duplicate']}, false {res['false']}, inversions {res['inversions']}")
        for o in res["rotations"]:
            print("   ", json.dumps(o))
    if a.json:
        a.json.write_text(json.dumps(allres, indent=1) + "\n")


if __name__ == "__main__":
    main()
