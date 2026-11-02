#!/usr/bin/env python3
"""Transport completeness at the laptop: of what the sources produced, what reached the laptop, in
order and in time. The model and the counter are the same for every method, so the reference is not
the ground truth but the results the camera and the helper really produced:

- reference: every camera result (serial.log [MOQ_TRACK], the published result: same captures and
  points as the subscriber receives) and every helper result (helper.log [HELPER_DET]), arriving at
  their capture time (a lossless, instant channel), counted by the laptop fuser (replay_fuser.replay);
- laptop: the results that reached the subscriber (sub-<sub>/events.jsonl) at their arrival times,
  counted by the same fuser.

Both are judged on the slot template (analyze_rotations.analyze) to cut them into rotations and slots.
A rotation is transport-complete when the laptop got every slot the reference got, no more duplicates
or false passages than the reference, no more inversions than the reference, and each of its passages
released within DEADLINE of its crossing (delay of replay_fuser.replay). Rotations are paired by their
start time.

Evidence: each passage the camera counted ([MOQ_PASSAGE]) has its evidence image, the group of its
frame (seq) on evidence/jpeg (alias 3), any representation. A rotation is mission-complete when it is
transport-complete and every camera passage crossing in it has its evidence at the laptop within
EVIDENCE_DEADLINE of its crossing (crossing: capture - (x - line) / mean vx of the template; host clock
mapped as replay_fuser.replay: the smallest arrival minus capture of the camera's results).

usage: eval_transport.py RUN... --template T [--camera-only] [--sub remote] [--deadline-s 5] [--evidence-deadline-s 10] [--wait-ms 1000]
                         [--horizon-ms 1000] [--speed fit-assoc] [--tie-s 0.16] [--json OUT]"""
import argparse
import json
import re
import tempfile
from pathlib import Path

import numpy as np

import analyze_rotations as A
import replay_fuser as R


def produced(run, helpers=True):
    out = []
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        if "[MOQ_TRACK] " in line:
            try:
                r = json.loads(line[line.index("{"):])
            except ValueError:
                continue
            out.append(dict(t=r["cap_ms"] / 1000, event="object", properties={"120": r["cap_ms"]},
                            result=dict(boxes=[dict(x=p[0], y=p[1]) for p in r["pts"]])))
    helper = run / "helper.log"
    for line in (helper.read_text(errors="replace").splitlines() if helper.exists() and helpers else []):
        if "[HELPER_DET] " in line:
            try:
                h = json.loads(line[line.index("{"):])
            except ValueError:
                continue
            if h.get("capture_ms"):
                out.append(dict(t=h["capture_ms"] / 1000, event="object", helper=h))
    out.sort(key=lambda r: r["t"])
    return out


def evidence(run, ev, tp):
    """-> [(crossing s, evidence delay s or None)] for the camera's passages."""
    cap_of, pas = {}, []
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        if "[MOQ_TRACK] " in line or "[MOQ_PASSAGE] " in line:
            try:
                r = json.loads(line[line.index("{"):])
            except ValueError:
                continue
            if "[MOQ_TRACK] " in line:
                cap_of[r["seq"]] = r["cap_ms"] / 1000
            elif r["seq"] in cap_of:
                pas.append((r["seq"], cap_of[r["seq"]] - (r["x"] - tp.line) / np.mean(tp.vx)))
    got, off = {}, None
    for line in open(ev):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        if r.get("event") not in ("object", "fetch_object"):   # evidence fetched by group counts at its arrival
            continue
        if r.get("event") == "fetch_object" and r.get("alias") != 3:
            continue
        shared = r.get("alias") == 1 and r.get("properties", {}).get("124") == 1   # evidence on the shared frame track
        if (r.get("alias") == 3 or shared) and "group" in r:
            got.setdefault(r["group"], r["t"])
        elif r.get("result") and "120" in r.get("properties", {}):
            d = r["t"] - r["properties"]["120"] / 1000
            off = d if off is None else min(off, d)
    return [(tc, got[s] - (tc + off) if s in got else None) for s, tc in pas]


def rotations(ev, tp, a):
    """-> {start time: (rotation dict, max delay of its passages, (lo, hi))}, fuser stats."""
    fused, st, delay, _ = R.replay(ev, a.wait_ms, a.horizon_ms, a.speed, False)
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        f.write("".join(json.dumps(o) + "\n" for o in fused))
    rows, span = A.load(f.name)
    Path(f.name).unlink()
    if len(rows) < tp.n:
        return {}, st
    off, P = A.fit(rows, tp)
    for _ in range(2):          # as analyze_rotations.analyze
        inst, T, C, match = A.assign(rows, tp, off, P, span)
        if len(match) < 3:
            break
        ks = np.array([inst[q][0] for q in match.values()], float)
        tc = np.array([rows[i]["cap"] - (rows[i]["x"] - tp.line) / tp.vx[inst[q][1]] - tp.phase[inst[q][1]]
                       for i, q in match.items()])
        off, P = np.linalg.lstsq(np.c_[np.ones(len(ks)), ks], tc, rcond=None)[0]
    r = A.analyze(rows, span, tp, a.tie_s)
    out = {}
    for o in r["rotations"] if r else []:
        start = off + o["rotation"] * P + tp.phase.min()
        lo, hi = start - 2, off + o["rotation"] * P + tp.phase.max() + 3
        d = [delay[i] for i, x in enumerate(fused) if lo <= x["crossing_ms"] / 1000 <= hi]
        out[start] = (o, max(d) if d else 0.0, (lo, hi))
    return out, st


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--template", type=Path, required=True)
    ap.add_argument("--sub", default="remote")
    ap.add_argument("--deadline-s", type=float, default=5.0)
    ap.add_argument("--evidence-deadline-s", type=float, default=10.0, help="the evidence deadline of the firmware (AIOT_MOQT_AI_DEADLINE_MS 10000)")
    ap.add_argument("--wait-ms", type=int, default=1000)
    ap.add_argument("--horizon-ms", type=int, default=1000)
    ap.add_argument("--speed", choices=("track", "fit", "fit-assoc"), default="fit-assoc")
    ap.add_argument("--tie-s", type=float, default=0.16)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--camera-only", action="store_true",
                    help="judge order and deadline on the camera's results only (the alerts): the helper's results are left out of the reference and of the laptop")
    a = ap.parse_args()
    tp = A.Template(a.template)
    out = []
    for run in a.runs:
        ev = run / f"sub-{a.sub}" / "events.jsonl"
        man = run / "manifest.json"
        if not ev.exists() or not man.exists() or '"complete"' not in man.read_text():
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write("".join(json.dumps(r) + "\n" for r in produced(run, not a.camera_only)))
        ref, rst = rotations(f.name, tp, a)
        Path(f.name).unlink()
        if a.camera_only:
            with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
                f.write("".join(l for l in ev.open() if '"helper"' not in l))
            lap, lst = rotations(f.name, tp, a)
            Path(f.name).unlink()
        else:
            lap, lst = rotations(ev, tp, a)
        evd = evidence(run, ev, tp)
        n = order = timely = mission = 0
        lost = []
        for s, (o, _, _) in ref.items():
            near = min(lap, key=lambda x: abs(x - s)) if lap else None
            if near is None or abs(near - s) > tp.P / 2:
                lost.append(o["rotation"])
                continue
            q, dmax, (lo, hi) = lap[near]
            n += 1
            got = set(range(tp.n)) - set(q["missing"]) >= set(range(tp.n)) - set(o["missing"])
            same = got and q["duplicate"] <= o["duplicate"] and q["false"] <= o["false"]
            order += same and q["inversions"] <= o["inversions"]
            ok = same and q["inversions"] <= o["inversions"] and dmax <= a.deadline_s
            timely += ok
            mine = [d for tc, d in evd if lo <= tc <= hi]
            mission += ok and all(d is not None and d <= a.evidence_deadline_s for d in mine)
        m = re.match(r"(\w+?)-(\w+)-(.+)-r(\d+)$", run.name)
        out.append(dict(run=run.name, situation=m.group(2) if m else "", method=m.group(3) if m else "",
                        rotations=len(ref), paired=n, complete_order=order, complete_deadline=timely,
                        complete_mission=mission, passages=len(evd),
                        evidence_delivered=sum(d is not None for _, d in evd),
                        evidence_in_time=sum(d is not None and d <= a.evidence_deadline_s for _, d in evd),
                        evidence_delay_p95_s=round(float(np.percentile([d for _, d in evd if d is not None], 95)), 2)
                        if any(d is not None for _, d in evd) else None,
                        camera_produced=rst["camera"], camera_delivered=lst["camera"],
                        helper_produced=rst["helper"], helper_delivered=lst["helper"], late=lst["late"],
                        ref_complete=int(sum(o["complete"] for o, _, _ in ref.values()))))
    print(f"{'run':30s} rot | complete: order deadline mission | evidence: passages deliv in-time p95 | camera prod/deliv | helper prod/deliv | late | ref complete")
    for r in out:
        print(f"{r['run']:30s} {r['rotations']:3d} | {r['complete_order']:5d} {r['complete_deadline']:8d} {r['complete_mission']:7d} | "
              f"{r['passages']:4d} {r['evidence_delivered']:5d} {r['evidence_in_time']:7d} {r['evidence_delay_p95_s']} | "
              f"{r['camera_produced']:4d}/{r['camera_delivered']:<4d}      | {r['helper_produced']:4d}/{r['helper_delivered']:<4d}      | "
              f"{r['late']:4d} | {r['ref_complete']}")
    if a.json:
        a.json.write_text(json.dumps(out, indent=1, default=int) + "\n")


if __name__ == "__main__":
    main()
