#!/usr/bin/env python3
"""Adapting the screw detector to a new installation, without a human in the loop:
recorded frames of the new scene -> automatic labels -> training -> held-out check -> asset -> image.

1. Frames: the frame traces of one or more runs in the new scene (moq/results/<run>/trace/frames,
   recorded with a trace build, e.g. moq/vendor/alert-state/trace_build.sh). The last run is held out
   (the check); with one run, its last HOLD share in time is held out instead.
2. Labels: label_turntable.py on each part (linked as frame-*.jpg in a session directory).
3. Training: train_turntable_fomo2.py, the training sessions of the base model plus the new ones;
   validation as the base model.
4. Check (eval_scene_shift.py, screw F1): the new model is accepted when it beats the base model on
   the held-out frames of the new scene by at least MIN_GAIN, and loses at most KEEP_DROP on each
   session kept for this (no forgetting of earlier scenes): the test sessions of the base model and
   every session of ST/labels-keep-*.jsonl (one recording per scene, never trained on; 10/7: s3b
   gained 0.004 on its scene and lost 0.17 of precision in scene 2, which only such a session shows).
5. Asset: screw_model_asset.h with the size of the int8 model and the figures of the check;
   with --build NAME, the image (moq/tools/build_model.sh, one build at a time under the lock).

usage: adapt_scene.py SCENE RUN [RUN ...] [--base model-c3] [--out model-NAME] [--rim PX] [--build NAME
                      [--method mus-alert] [-D...]] [--steps 6000]"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MOQ = HERE.parent / "moq"
DATA = Path("/home/ryzen395/data/aiot_quic_results")
ST = DATA / "screw_turntable"
HOLD = 0.35
KEEP_DROP = 0.01
MIN_GAIN = 0.01
PY = sys.executable


def run(cmd, log=None, **kw):
    print("+", " ".join(map(str, cmd)), flush=True)
    if log:
        with open(log, "w") as f:
            subprocess.run(list(map(str, cmd)), check=True, stdout=f, stderr=subprocess.STDOUT, **kw)
    else:
        subprocess.run(list(map(str, cmd)), check=True, **kw)


def sessions(scene, runs, root):
    """{session name: [frame paths]}, the last entry held out."""
    parts = []
    for r in runs:
        fr = sorted((DATA / "moq" / r / "trace" / "frames").glob("*.jpg"))
        if not fr:
            sys.exit(f"{r}: no trace frames")
        parts.append((r, fr))
    if len(parts) == 1:
        r, fr = parts[0]
        k = int(len(fr) * (1 - HOLD))
        parts = [(f"{r}-fit", fr[:k]), (f"{r}-held", fr[k:])]
    out = {}
    for name, fr in parts:
        d = root / f"{scene}-{name}"
        d.mkdir(parents=True, exist_ok=True)
        for i, f in enumerate(fr):
            link = d / f"frame-{i:06d}.jpg"
            if not link.exists():
                link.symlink_to(f)
        out[d.name] = d
    return out


def label_files_for(names):
    """label files of ST holding the given sessions."""
    want, files = set(names), []
    for f in sorted(ST.glob("labels-*.jsonl")):
        have = {json.loads(l)["session"] for l in open(f)}
        if have & want:
            files.append(f)
            want -= have
    if want:
        sys.exit(f"no labels for sessions {sorted(want)}")
    return files


def subset(files, session, path):
    rows = [l for f in files for l in open(f) if json.loads(l)["session"] == session]
    path.write_text("".join(rows))
    return path


def f1(model, labels):
    out = subprocess.run([PY, HERE / "eval_scene_shift.py", labels, "--model", model],
                         check=True, capture_output=True, text=True).stdout
    print("   ", out.strip(), flush=True)
    r = out.split("recall ")[1]
    rc, pr = float(r.split(",")[0]), float(r.split("precision ")[1].split()[0])
    return 2 * rc * pr / max(rc + pr, 1e-9), rc, pr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scene")
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--base", default="model-c3")
    ap.add_argument("--out")
    ap.add_argument("--steps", type=int, default=6000)
    ap.add_argument("--build")
    ap.add_argument("--method", default="mus-alert")
    ap.add_argument("--rim", type=int, default=0, help="label_turntable.py --rim (screws lying on the plate rim)")
    a, extra = ap.parse_known_args()
    base = ST / a.base
    out = ST / (a.out or f"model-{a.scene}")
    work = ST / "adapt" / a.scene
    work.mkdir(parents=True, exist_ok=True)
    bt = json.loads((base / "training.json").read_text())

    # 1-2: frames and automatic labels
    parts = sessions(a.scene, a.runs, work)
    labels = ST / f"labels-{a.scene}.jsonl"
    run([PY, HERE / "label_turntable.py", *parts.values(), "--out", labels, "--sheet", work / "review.jpg",
         "--rim", a.rim])
    new = list(parts)
    fit, held = new[:-1], new[-1]

    # 3: training
    train = bt["train"] + [s for s in fit if s not in bt["train"]]
    keep = sorted({json.loads(l)["session"] for f in ST.glob("labels-keep-*.jsonl") for l in open(f)})
    assert not set(keep) & set(train + bt["val"]), "a kept session is in training or validation"
    lab = label_files_for(train + bt["val"] + bt.get("test", []) + [held])
    keep_lab = label_files_for(keep) if keep else []
    env = dict(os.environ, HSA_OVERRIDE_GFX_VERSION="11.0.0")
    run(["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=6G", "nice", PY, HERE / "train_turntable_fomo2.py",
         "--labels", *lab, "--train", *train, "--val", *bt["val"], "--test", held, *bt.get("test", []),
         "--out", out, "--steps", a.steps], log=ST / f"train-{out.name}.log", env=env)

    # 4: check
    tfl = "screw_turntable_int8.tflite"
    report = {}
    for s in [held] + [t for t in bt.get("test", []) + keep if t != held]:
        p = subset(lab + keep_lab, s, work / f"check-{s}.jsonl")
        print(f"check {s}", flush=True)
        report[s] = dict(base=f1(base / tfl, p), new=f1(out / tfl, p))
    gain = report[held]["new"][0] - report[held]["base"][0]
    kept = all(r["new"][0] >= r["base"][0] - KEEP_DROP for s, r in report.items() if s != held)
    accept = gain >= MIN_GAIN and kept
    (out / "adapt.json").write_text(json.dumps(dict(
        scene=a.scene, runs=a.runs, base=a.base, held_out=held, check=report, accept=accept), indent=1) + "\n")
    print(f"held-out {held}: F1 {report[held]['base'][0]:.3f} -> {report[held]['new'][0]:.3f}; "
          f"(at least {MIN_GAIN}); earlier scenes kept: {kept}; {'ACCEPT' if accept else 'REJECT'}", flush=True)
    if not accept:
        sys.exit(1)

    # 5: asset and image
    size = (out / tfl).stat().st_size
    _, rc, pr = report[held]["new"]
    hdr = (base / "screw_model_asset.h").read_text()
    head, tail = hdr.split("/*", 1)[0], hdr.split("*/", 1)[1]
    note = (f"/* Turntable screw model ({out.name}: experiments/screw_mission/adapt_scene.py from {a.base}, design \"fast\"\n"
            f" * with a count head; train_turntable_fomo2.py). Adapted to scene {a.scene} with automatic labels of\n"
            f" * {', '.join(fit)}; held-out {held}: recall {rc:.3f}, precision {pr:.3f} (eval_scene_shift.py).\n"
            f" * Threshold 0.3 (77/255) on 1 - background.\n */")
    import re
    tail = re.sub(r"\.len = \d+U", f".len = {size}U", tail)
    (out / "screw_model_asset.h").write_text(head + note + tail)
    if a.build:
        run(["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=6G", "nice", "flock", "vendor/build.lock",
             "tools/build_model.sh", a.method, out, a.build, *extra], cwd=MOQ)


if __name__ == "__main__":
    main()
