#!/usr/bin/env python3
"""What the laptop gains by recounting the screws of a passage on its evidence frame (offline, from a trace run).

The device counts on a 1/8 decode (200x150) into a 20x15 grid: screws that lie closer than one cell at the
line (scene 3: a cluster of three at 9.7 s, a close pair at 16.4 s, a pair touching on the rim at 22.4 s)
give fewer passages than screws. The evidence track (evidence/jpeg, 400x300, ~9 KB) carries the frame of a
sample; the laptop can count the dark regions near each passage on it, model-free (label_turntable.py, its
screw area per row fitted on the evidence frames themselves), and add the passages the device missed.

Simulation on a trace run (serial.log passages + trace/frames):
- evidence of a passage sample: the trace frame of the same capture, or the nearest one within MAX_DT
  (positions moved back by the plate speed), resized to 400x300 and JPEG-encoded at QUALITY;
- recount: the labelled screws within WIN of a device passage of that sample form its cluster; each one
  not within MATCH of a device passage of the sample, and inside the band where passages are reported,
  is a new passage of that sample;
- judged before and after on the slot template (analyze_rotations.analyze).

usage: evidence_recount_sim.py RUN [--template T] [--rim 10] [--work DIR]"""
import argparse
import csv
import io
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import analyze_rotations as A

HERE = Path(__file__).resolve().parent
LABEL = HERE.parent / "screw_mission" / "label_turntable.py"
MAX_DT = 0.2
QUALITY = 40
WIN = 12          # 256 units around a device passage
MATCH = 6
BAND = (120, 160)  # x of reported passages
VX = 14.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=Path)
    ap.add_argument("--template", type=Path)
    ap.add_argument("--rim", type=int, default=10)
    ap.add_argument("--work", type=Path)
    a = ap.parse_args()
    tp = A.Template(a.template or a.run / "slot_template.json")
    rows, span = A.load(a.run / "serial.log")
    seq_of = []
    for line in (a.run / "serial.log").read_text(errors="replace").splitlines():
        if "[MOQ_PASSAGE] " in line:
            try:
                seq_of.append(json.loads(line[line.index("{"):])["seq"])
            except ValueError:
                pass
    # rows of A.load are the passages whose seq has a track line, in file order: pair them with seq_of
    cap_of = {}
    for line in (a.run / "serial.log").read_text(errors="replace").splitlines():
        if "[MOQ_TRACK] " in line:
            try:
                r = json.loads(line[line.index("{"):])
                cap_of[r["seq"]] = r["cap_ms"] / 1000
            except ValueError:
                pass
    seqs = [s for s in seq_of if s in cap_of]
    assert len(seqs) == len(rows)
    for r, s in zip(rows, seqs):
        r["seq"] = s
    idx = [(int(q[0]), int(q[1]) / 1e6) for q in csv.reader(open(a.run / "trace" / "index.csv"))]
    have = {int(p.stem) for p in (a.run / "trace" / "frames").glob("*.jpg")}
    idx = [(s, t) for s, t in idx if s in have]
    ts = np.array([t for _, t in idx])
    work = a.work or a.run / "evidence-sim"
    sess = work / "evidence"
    sess.mkdir(parents=True, exist_ok=True)
    ev = {}
    sizes = []
    for s in sorted(set(seqs)):
        t = cap_of[s]
        k = int(np.argmin(np.abs(ts - t)))
        if abs(ts[k] - t) > MAX_DT:
            continue
        f = sess / f"frame-{s:06d}.jpg"
        if not f.exists():
            im = Image.open(a.run / "trace" / "frames" / f"{idx[k][0]:07d}.jpg").convert("RGB").resize((400, 300), Image.BILINEAR)
            buf = io.BytesIO()
            im.save(buf, "JPEG", quality=QUALITY)
            f.write_bytes(buf.getvalue())
        sizes.append(f.stat().st_size)
        ev[s] = ts[k] - t
    labels = work / "labels.jsonl"
    subprocess.run([sys.executable, LABEL, sess, "--out", labels, "--rim", str(a.rim)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    objs = {}
    for line in open(labels):
        r = json.loads(line)
        s = int(Path(r["frame"]).stem.split("-")[1])
        dt = ev[s]
        objs[s] = [(o["cx"] * 256 - VX * dt, o["cy"] * 256) for o in r["objects"]]
    added = []
    by_seq = {}
    for r in rows:
        by_seq.setdefault(r["seq"], []).append(r)
    for s, ps in by_seq.items():
        if s not in objs:
            continue
        P = np.array([(p["x"], p["y"]) for p in ps], float)
        for ox, oy in objs[s]:
            if not BAND[0] <= ox <= BAND[1]:
                continue
            d = np.max(np.abs(P - (ox, oy)), 1)
            if d.min() <= MATCH or d.min() > WIN:
                continue          # the device has it, or it is no neighbour of a device passage
            added.append(dict(arrival=ps[-1]["arrival"], rank=ps[-1]["rank"], cap=ps[0]["cap"], x=round(ox), y=round(oy),
                              seq=s, added=True))
    keys = ("n", "complete_rotations", "slot_recall", "missing", "duplicate", "false", "inversions")
    before = A.analyze(rows, span, tp, 0.16)
    after = A.analyze(sorted(rows + added, key=lambda r: (r["arrival"], r["rank"])), span, tp, 0.16)
    from collections import Counter
    out = dict(run=a.run.name, passage_samples=len(by_seq), evidence=len(ev), evidence_bytes_mean=int(np.mean(sizes)),
               added=len(added), before={k: before[k] for k in keys}, after={k: after[k] for k in keys},
               missing_before=dict(Counter(j for o in before["rotations"] for j in o["missing"])),
               missing_after=dict(Counter(j for o in after["rotations"] for j in o["missing"])))
    (work / "result.json").write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
