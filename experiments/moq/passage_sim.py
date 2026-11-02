#!/usr/bin/env python3
"""Operating range of the passage counter of the firmware (src/main/moq_passage.c), by simulation.

Objects move along x in one lane and cross the counting line. The counter receives one set of
detections per inference result. Swept: the distance an object moves between two results,
the probability that a detection is missing, and the spacing of the objects. The count of
the firmware code is compared with the number of objects that crossed."""

import argparse
import ctypes
import json
import random
import statistics
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRACKS, EVENTS = 24, 8


class Cfg(ctypes.Structure):
    _fields_ = [("line", ctypes.c_uint8), ("vertical_axis", ctypes.c_bool), ("direction", ctypes.c_int8),
                ("max_step", ctypes.c_uint8), ("max_missed", ctypes.c_uint8), ("predict", ctypes.c_bool),
                ("count_unseen", ctypes.c_bool)]


class Track(ctypes.Structure):
    _fields_ = [("used", ctypes.c_bool), ("counted", ctypes.c_bool), ("moving", ctypes.c_bool), ("x", ctypes.c_uint8),
                ("y", ctypes.c_uint8), ("missed", ctypes.c_uint8), ("v", ctypes.c_int16)]


class Counter(ctypes.Structure):
    _fields_ = [("cfg", Cfg), ("tracks", Track * TRACKS), ("flow", ctypes.c_int16), ("has_flow", ctypes.c_bool),
                ("total", ctypes.c_uint32), ("dropped_detections", ctypes.c_uint32)]


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_uint8), ("y", ctypes.c_uint8)]


def library():
    out = Path(tempfile.mkdtemp()) / "passage.so"
    src = ROOT / "src/main"
    subprocess.run(["cc", "-O2", "-shared", "-fPIC", f"-I{src}", str(src / "moq_passage.c"), "-o", str(out)], check=True)
    return ctypes.CDLL(str(out))


def run(lib, *, step, miss, spacing, objects, noise, max_step, max_missed, seed, predict=False, unseen=False):
    rng = random.Random(seed)
    counter = Counter()
    cfg = Cfg(128, False, 1, max_step, max_missed, predict, unseen)
    lib.moq_passage_init(ctypes.byref(counter), ctypes.byref(cfg))
    # Object k enters at x = 0 at the time k * spacing / step, in results.
    crossed, t = 0, 0
    phase = [rng.uniform(0, step) for _ in range(objects)]
    while True:
        points, alive = [], False
        for k in range(objects):
            x = (t * step) - k * spacing + phase[k]
            if x < 0:
                alive = True
                continue
            if x > 255:
                continue
            alive = True
            if rng.random() < miss:
                continue
            px = min(255, max(0, round(x + rng.gauss(0, noise))))
            py = min(255, max(0, round(128 + rng.gauss(0, noise))))
            points.append(Point(px, py))
        if not alive:
            break
        arr = (Point * max(1, len(points)))(*points)
        lib.moq_passage_update(ctypes.byref(counter), arr, len(points), t, None)
        t += 1
    crossed = objects
    return counter.total, crossed


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--objects", type=int, default=200)
    p.add_argument("--seeds", type=int, default=20)
    p.add_argument("--max-step", type=int, default=48)
    p.add_argument("--max-missed", type=int, default=2)
    p.add_argument("--noise", type=float, default=3.0)
    a = p.parse_args()
    lib = library()
    lib.moq_passage_update.restype = ctypes.c_uint
    rows = []
    methods = dict(nearest=dict(predict=False, unseen=False), predict=dict(predict=True, unseen=False),
                   predict_unseen=dict(predict=True, unseen=True))
    grid = [(sp, st, m) for sp in (64, 96, 128) for st in (8, 16, 24, 32, 40, 48) for m in (0.0, 0.1, 0.2, 0.3)]
    for name, method in methods.items():
        for spacing, step, miss in grid:
            errors = []
            for seed in range(a.seeds):
                counted, true = run(lib, step=step, miss=miss, spacing=spacing, objects=a.objects, noise=a.noise,
                                    max_step=a.max_step, max_missed=a.max_missed, seed=seed, **method)
                errors.append(100 * (counted - true) / true)
            rows.append(dict(method=name, spacing=spacing, step=step, miss=miss, error_percent_mean=statistics.mean(errors),
                             error_percent_min=min(errors), error_percent_max=max(errors)))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "passage-sim.json").write_text(json.dumps(dict(
        schema=2, kind="simulation of src/main/moq_passage.c", objects=a.objects, seeds=a.seeds, max_step=a.max_step,
        max_missed=a.max_missed, noise=a.noise, line=128, rows=rows), indent=2) + "\n")
    for name in methods:
        for spacing in (64, 96, 128):
            print(f"{name}, spacing {spacing}: count error %, mean of {a.seeds} seeds")
            for step in (8, 16, 24, 32, 40, 48):
                cells = [r for r in rows if r["method"] == name and r["spacing"] == spacing and r["step"] == step]
                print(f"{step:4d}  " + "  ".join(f"{r['error_percent_mean']:+7.1f}" for r in cells))


if __name__ == "__main__":
    main()
