#!/usr/bin/env python3
"""Frozen-policy remote comparison; stop at the first failed measurement."""
import argparse
import fcntl
import hashlib
import json
import random
import signal
import subprocess
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
PROFILES = {"good": (2048, 5, 0), "medium": (512, 25, 1), "poor": (128, 75, 3)}
POLICIES = ("low", "medium", "ewma", "swma")


def plan(seed, repeats, current):
    rng = random.Random(seed)
    cells = []
    for repeat in range(1, repeats + 1):
        block = [dict(policy=p, network=n, repeat=repeat) for p in POLICIES for n in PROFILES]
        for _ in range(10000):
            rng.shuffle(block)
            sequence = [current] + [x["policy"] for x in block]
            if all(a != b for a, b in zip(sequence, sequence[1:])):
                break
        else:
            raise RuntimeError("unable to schedule distinct adjacent firmware identities")
        cells.extend(block)
        current = block[-1]["policy"]
    return cells


def save(path, data):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--seed", type=int, default=20260926)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--current-policy", choices=POLICIES, required=True)
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("repeats must be positive")
    lock = (BASE / "results/remote-comparison.lock").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    out = BASE / "results" / args.name
    out.mkdir(exist_ok=False)
    firmware = {}
    for policy in POLICIES:
        path = BASE / "results/literature-v25-firmware" / policy / "firmware.bin"
        raw = path.read_bytes()
        provenance = json.loads(path.with_name("provenance.json").read_text())
        digest = hashlib.sha256(raw).hexdigest()
        if digest != provenance["firmware_sha256"]:
            raise ValueError(f"frozen firmware hash mismatch: {policy}")
        firmware[policy] = dict(path=str(path), sha256=digest, elf_sha256=raw[176:208].hex(), provenance=provenance)
    state = dict(status="planned", seed=args.seed, scope="replay transport; no physical recall claim",
                 firmware=firmware, profiles=PROFILES,
                 cells=plan(args.seed, args.repeats, args.current_policy), results=[])
    save(out / "manifest.json", state)
    if not args.execute:
        print(out)
        return
    child = None

    def stop(signum, _frame):
        raise RuntimeError(f"suite interrupted: {signum}")

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        for index, cell in enumerate(state["cells"]):
            name = f"{args.name}-{cell['policy']}-{cell['network']}-r{cell['repeat']}"
            rate, delay, loss = PROFILES[cell["network"]]
            state.update(status="running", active=name, completed=len(state["results"]))
            save(out / "manifest.json", state)
            cmd = [sys.executable, str(BASE / "network_only_trial.py"), "--name", name,
                   "--firmware", firmware[cell["policy"]]["path"], "--rate-kbit", str(rate),
                   "--delay-ms", str(delay), "--loss-percent", str(loss), "--seconds", "180"]
            with (out / f"{index:02d}.log").open("w") as log:
                child = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                code = child.wait(timeout=480)
            run = json.loads((BASE / "results" / name / "manifest.json").read_text())
            if code or run["status"] != "complete" or run.get("post_run_fresh_polls") != 2:
                raise RuntimeError(f"trial failed: {name}: {run.get('reason', code)}")
            state["results"].append(dict(**cell, run=name, summary=run["summary"]))
            state["completed"] = len(state["results"])
            save(out / "manifest.json", state)
            print(f"completed {state['completed']}/{len(state['cells'])}: {name}", flush=True)
        state.update(status="complete", active=None)
    except Exception as exc:
        state.update(status="stopped", reason=str(exc))
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=25)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                state["cleanup_warning"] = "child killed; inspect reserved shaper before restarting"
        save(out / "manifest.json", state)
    sys.exit(0 if state["status"] == "complete" else 1)


if __name__ == "__main__":
    main()
