#!/usr/bin/env python3
"""Sequential, randomized OTA trials with per-run restoration and frozen inputs."""

import argparse
import hashlib
import json
import random
import signal
import subprocess
from pathlib import Path

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
PYTHON = BASE / "vendor/venv/bin/python"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--name", required=True)
    p.add_argument(
        "--policies", nargs="+", default=["high", "high_timeout", "low", "medium", "adaptive"]
    )
    p.add_argument("--networks", nargs="+", choices=["128", "256", "512", "step", "good", "medium", "poor"], default=["128"])
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--seed", type=int, default=20260923)
    p.add_argument("--firmware-root", type=Path, default=BASE / "results/ladder-v17-firmware")
    p.add_argument(
        "--evidence", action="store_true", help="on-device codec and exact-source evidence cohort"
    )
    p.add_argument("--execute", action="store_true")
    p.add_argument("--restore-image", type=Path,
                   help="Explicit archived recovery image for this cohort")
    a = p.parse_args()
    if a.repeats < 1:
        p.error("positive repetitions required")
    directory = BASE / "results" / a.name
    directory.mkdir(exist_ok=False)
    firmware_root = a.firmware_root
    firmware = {}
    for policy in a.policies:
        path = firmware_root / policy / "firmware.bin"
        if not path.is_file():
            p.error(f"missing firmware: {path}")
        firmware[policy] = dict(path=str(path), sha256=sha(path))
        provenance = path.parent / "provenance.json"
        if provenance.is_file():
            firmware[policy]["provenance_sha256"] = sha(provenance)
            firmware[policy]["configuration"] = json.loads(provenance.read_text()).get(
                "configuration", {}
            )
    ladder = BASE / "fixtures/screw-ladder-v1/manifest.json"
    profiles = {
        "good": dict(rate_kbit=2048, delay_ms=5, loss_percent=0),
        "medium": dict(rate_kbit=512, delay_ms=25, loss_percent=1),
        "poor": dict(rate_kbit=128, delay_ms=75, loss_percent=3),
    }
    rng = random.Random(a.seed)
    cells = []
    # Block randomization prevents policy and time-of-day being confounded.
    for repeat in range(a.repeats):
        block = [
            dict(policy=policy, network=network, repeat=repeat + 1)
            for policy in a.policies
            for network in a.networks
        ]
        rng.shuffle(block)
        cells.extend(block)
    manifest = dict(
        schema=1,
        scope="static replay transport characterization, not physical screw mission",
        seed=a.seed,
        network_profiles=profiles,
        network_scope="Host ingress impairment of device UDP flow; not RF signal control",
        ladder=json.loads(ladder.read_text()),
        ladder_sha256=sha(ladder),
        firmware=firmware,
        cells=cells,
        results=[],
        status="planned",
        controls=dict(
            ai_input="unchanged high.jpg",
            ai_deadline_ms=10000,
            preview_deadline_ms=2000,
            slots=1,
            unacked_cap_kib=32,
            encoding="offline for ALL variants",
            measurement_seconds=60,
            drain_seconds=10,
            selector="S1 AI-first",
            rate_estimator="ACK service EWMA; prior 64000 B/s; no shaper oracle",
        ),
    )
    if a.restore_image:
        if not a.restore_image.is_file() or a.restore_image.stem != a.restore_image.read_bytes()[176:208].hex():
            p.error("restore image must exist and be named by its ELF SHA256")
        manifest["restore_image"] = dict(path=str(a.restore_image.resolve()), sha256=sha(a.restore_image))
    if a.evidence:
        manifest["scope"] = (
            "static replay with on-device codec and exact-source result/evidence pairing; not physical GT"
        )
        manifest["controls"].update(
            encoding="on-device",
            image_role="event evidence",
            image_deadline_ms=10000,
            preview_deadline_ms=None,
        )
    if any(
        "AIOT_MOQT_RATE_ESTIMATOR:STRING" in f.get("configuration", {}) for f in firmware.values()
    ):
        manifest["controls"]["rate_estimator"] = (
            "per-firmware frozen configuration: 0 ACK-service EWMA, 1 SWMAth math on ACK service; no shaper oracle"
        )

    def save():
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    save()
    if not a.execute:
        print(directory / "manifest.json")
        return
    child = None
    stopping = False

    def stop(sig, frame):
        nonlocal stopping
        if stopping:
            return
        stopping = True
        if child and child.poll() is None:
            child.terminate()
        # Do not wait inside a signal handler: it can interrupt Popen.wait while
        # its non-reentrant waitpid lock is held. Unwind first, then join below.
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    manifest["status"] = "running"
    save()
    try:
        for cell in cells:
            info = firmware[cell["policy"]]
            path = Path(info["path"])
            if sha(path) != info["sha256"]:
                raise RuntimeError("firmware changed after freezing suite")
            run = f"{a.name}-{cell['policy']}-{cell['network']}-r{cell['repeat']}"
            profile = profiles.get(cell["network"], dict(rate_kbit=512 if cell["network"] == "step" else cell["network"], delay_ms=50, loss_percent=0))
            command = [
                str(PYTHON),
                str(BASE / "remote_ota_trial.py"),
                "--name",
                run,
                "--firmware",
                str(path),
                "--shaped",
                "--rate-kbit",
                str(profile["rate_kbit"]),
                "--delay-ms",
                str(profile["delay_ms"]),
                "--loss-percent",
                str(profile["loss_percent"]),
                "--timeout",
                "180",
            ]
            if cell["network"] == "step":
                command.append("--step-profile")
            if a.restore_image:
                if sha(a.restore_image) != manifest["restore_image"]["sha256"]:
                    raise RuntimeError("restore image changed after freezing suite")
                command.extend(["--restore-image", str(a.restore_image.resolve())])
            record = dict(**cell, run=run, status="running")
            manifest["results"].append(record)
            save()
            print(json.dumps(record), flush=True)
            with (directory / (run + ".log")).open("w") as handle:
                child = subprocess.Popen(command, cwd=ROOT, stdout=handle, stderr=subprocess.STDOUT)
                code = child.wait()
            trial = json.loads((BASE / "results" / run / "manifest.json").read_text())
            record.update(
                status=trial["status"],
                returncode=code,
                restore_verified=trial.get("restore_verified"),
            )
            save()
            print(json.dumps(record), flush=True)
            if code or not record["restore_verified"]:
                raise RuntimeError("trial failed; retain failed run, stop before next OTA")
        manifest["status"] = "complete"
    except BaseException as exc:
        manifest["status"] = "stopped"
        manifest["reason"] = str(exc)
        raise
    finally:
        try:
            if child is not None:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=480)  # child restores in its finally block
                if manifest["results"]:
                    record = manifest["results"][-1]
                    trial_path = BASE / "results" / record["run"] / "manifest.json"
                    if trial_path.is_file():
                        trial = json.loads(trial_path.read_text())
                        record.update(
                            status=trial["status"],
                            returncode=child.returncode,
                            restore_verified=trial.get("restore_verified"),
                        )
        except BaseException as exc:
            manifest["cleanup_error"] = str(exc)
            manifest["status"] = "stopped"
            raise
        finally:
            save()


if __name__ == "__main__":
    main()
