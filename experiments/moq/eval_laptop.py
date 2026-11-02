#!/usr/bin/env python3
"""The mission at the laptop for runs of one camera (no helper): the results that reached a
subscriber (default remote, the laptop over the Internet: sub-remote/events.jsonl) counted by the
ordered fuser of the laptop (replay_fuser.replay, --speed, --confirm, WAIT and H) at their arrival
times, judged on a slot template (analyze_rotations.analyze); next to it the counter of the device
(serial.log) on the same template.

usage: eval_laptop.py RUN... --template T [--sub remote] [--wait-ms 1000] [--horizon-ms 1000]
                      [--speed fit-assoc] [--confirm] [--tie-s 0.16] [--json OUT]"""
import argparse
import json
import re
import tempfile
from pathlib import Path

import numpy as np

import analyze_rotations as A
import replay_fuser as R

KEYS = ("n", "complete_rotations", "slot_recall", "missing", "duplicate", "false", "inversions")


def judge(path, tp, tie):
    rows, span = A.load(path)
    r = A.analyze(rows, span, tp, tie) if rows else None
    return {k: r[k] for k in KEYS} if r else dict(n=0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", type=Path)
    ap.add_argument("--template", type=Path, required=True)
    ap.add_argument("--sub", default="remote")
    ap.add_argument("--wait-ms", type=int, default=1000)
    ap.add_argument("--horizon-ms", type=int, default=1000)
    ap.add_argument("--speed", choices=("track", "fit", "fit-assoc"), default="fit-assoc")
    ap.add_argument("--confirm", action="store_true")
    ap.add_argument("--tie-s", type=float, default=0.16)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    tp = A.Template(a.template)
    out = []
    for run in a.runs:
        ev = run / f"sub-{a.sub}" / "events.jsonl"
        man = run / "manifest.json"
        if not ev.exists() or not man.exists() or '"complete"' not in man.read_text():
            continue
        fused, st, delay, _ = R.replay(ev, a.wait_ms, a.horizon_ms, a.speed, a.confirm)
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write("".join(json.dumps(o) + "\n" for o in fused))
        laptop = judge(f.name, tp, a.tie_s)
        Path(f.name).unlink()
        device = judge(run / "serial.log", tp, a.tie_s)
        m = re.match(r"(\w+?)-(\w+)-(.+)-r(\d+)$", run.name)
        out.append(dict(run=run.name, situation=m.group(2) if m else "", method=m.group(3) if m else "",
                        samples=st["camera"], late=st["late"], laptop=laptop, device=device,
                        delay_p50_s=round(float(np.median(delay)), 2) if delay else None,
                        delay_p95_s=round(float(np.percentile(delay, 95)), 2) if delay else None))
    print(f"{'run':30s} {'samp':>4s} | laptop: compl recall miss dup false inv | device: compl recall miss dup false inv | delay p50/p95")
    for r in out:
        L, D = r["laptop"], r["device"]
        f = lambda X: (f"{X.get('complete_rotations', 0)}/{X['n']:<2d} {X.get('slot_recall') or 0:6.3f} {X.get('missing', '-'):>4} "
                       f"{X.get('duplicate', '-'):>3} {X.get('false', '-'):>5} {X.get('inversions', '-'):>3}")
        print(f"{r['run']:30s} {r['samples']:4d} | {f(L)} | {f(D)} | {r['delay_p50_s']}/{r['delay_p95_s']}")
    if a.json:
        a.json.write_text(json.dumps(out, indent=1) + "\n")


if __name__ == "__main__":
    main()
