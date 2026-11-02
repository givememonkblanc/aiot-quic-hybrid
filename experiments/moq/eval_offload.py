#!/usr/bin/env python3
"""Table of the offload study: per run, the mission at the laptop judged on the stream of the live
fuser of the run (fused.jsonl) and on the ordered fuser replayed at the recorded arrival times
(replay_fuser.py, --speed, WAIT and H below), with the concurrency tolerance of the ground truth
(--tie-s, one frame interval of the trace), and the offload counters of the camera.

usage: eval_offload.py results/off1-* [--wait-ms 1000] [--horizon-ms 1000] [--tie-s 0.16] [--speed fit|fit-assoc] [--confirm] [--template T] [--json OUT]"""
import argparse
import json
import re
import tempfile
from pathlib import Path

import numpy as np

import analyze_rotations as A
import replay_fuser as R
from fuse_offload import tagged


def judge(path, tp, tie):
    rows, span = A.load(path)
    r = A.analyze(rows, span, tp, tie)
    if not r:
        return dict(n=0)
    return {k: r[k] for k in ("n", "complete_rotations", "slot_recall", "missing", "duplicate", "false", "inversions")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--wait-ms", type=int, default=1000)
    ap.add_argument("--horizon-ms", type=int, default=1000)
    ap.add_argument("--tie-s", type=float, default=0.16)
    ap.add_argument("--speed", choices=("track", "fit", "fit-assoc"), default="fit")
    ap.add_argument("--confirm", action="store_true", help="count a track born past the line only once seen again")
    ap.add_argument("--template", type=Path, help="slot template (default: the newest results/trace-*/slot_template.json)")
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    tp = A.Template(a.template or A.newest_template())
    rows = []
    for run in a.runs:
        ev = run / "sub-qa" / "events.jsonl"
        if not ev.exists() or not (run / "fused.jsonl").exists():
            continue
        m = re.match(r"off\d+-(\w+)-(\w+)-r(\d+)", run.name)
        live = judge(run / "fused.jsonl", tp, a.tie_s)
        out, st, delay, _ = R.replay(ev, a.wait_ms, a.horizon_ms, a.speed, a.confirm)
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write("".join(json.dumps(o) + "\n" for o in out))
        ordered = judge(f.name, tp, a.tie_s)
        Path(f.name).unlink()
        offered = tagged(run / "serial.log", "MOQ_OFFLOAD")
        rows.append(dict(run=run.name, situation=m.group(1) if m else "", method=m.group(2) if m else "",
                         round=int(m.group(3)) if m else 0, offered=len(offered),
                         helper_samples=st["helper"], camera_samples=st["camera"], late=st["late"],
                         live=live, ordered=ordered,
                         delay_p50_s=round(float(np.median(delay)), 2) if delay else None,
                         delay_p95_s=round(float(np.percentile(delay, 95)), 2) if delay else None))
    order = {"local": 0, "naive": 1, "mbm": 2, "mbm2": 3}
    rows.sort(key=lambda r: (r["situation"], order.get(r["method"], 9), r["round"]))
    print(f"{'run':24s} {'off':>4s} {'help':>4s} | live: compl  miss dup inv | ordered: compl  recall miss dup inv | delay p50/p95")
    for r in rows:
        L, O = r["live"], r["ordered"]
        print(f"{r['run']:24s} {r['offered']:4d} {r['helper_samples']:4d} | {L.get('complete_rotations', 0)}/{L['n']:<2d} "
              f"{L.get('missing', '-'):>5} {L.get('duplicate', '-'):>3} {L.get('inversions', '-'):>3} | "
              f"{O.get('complete_rotations', 0)}/{O['n']:<2d} {O.get('slot_recall') or 0:7.3f} {O.get('missing', '-'):>4} "
              f"{O.get('duplicate', '-'):>3} {O.get('inversions', '-'):>3} | {r['delay_p50_s']}/{r['delay_p95_s']}")
    if a.json:
        a.json.write_text(json.dumps(rows, indent=1) + "\n")


if __name__ == "__main__":
    main()
