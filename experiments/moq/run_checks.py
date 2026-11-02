#!/usr/bin/env python3
"""Host-only checks. No flashing, network shaping, or device configuration."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[1]
TESTS = BASE / "tests"


def run(command, cwd=ROOT):
    subprocess.run([str(part) for part in command], cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--literature",
        action="store_true",
        help="also compare SWMAth/IFA to pinned upstream JavaScript; requires Node and vendor checkout",
    )
    args = parser.parse_args()
    transport = ROOT / "src/components/transport"
    main_source = ROOT / "src/main"
    cases = {
        "wire": ([transport / "moqt21.c"], [transport], []),
        "codec": ([transport / "moqt21.c"], [transport], []),
        "core": ([transport / "moqt_core.c", transport / "moqt21.c"], [transport], []),
        "scheduler": ([transport / "moqt_scheduler.c"], [transport], []),
        "tx_policy": ([], [transport], []),
        "feasibility": ([], [transport], []),
        "passage": ([main_source / "moq_passage.c"], [main_source], []),
        "mission_sched": ([main_source / "moq_mission_sched.c"], [main_source], []),
        "representation": (
            [main_source / "moq_representation.c"],
            [main_source, TESTS / "codec_test_stubs"],
            [],
        ),
        "codec_worker": (
            [main_source / "moq_codec_worker.c"],
            [main_source, TESTS / "worker_test_stubs"],
            ["-pthread"],
        ),
    }
    with tempfile.TemporaryDirectory(prefix="moq-checks-") as directory:
        for name, (sources, includes, options) in cases.items():
            executable = Path(directory) / name
            print(f"Checking C: {name}", flush=True)
            command = [
                "gcc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                *options,
            ]
            command += [f"-I{path}" for path in includes]
            command += [TESTS / f"test_{name}.c", *sources, "-o", executable]
            run(command)
            run([executable])
    run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"], cwd=BASE)
    run(
        [sys.executable, "-m", "unittest", "test_mission_metrics", "test_spatial_metrics"],
        cwd=ROOT / "experiments/screw_mission",
    )
    if args.literature:
        run([sys.executable, BASE / "literature/verify_estimators.py"])


if __name__ == "__main__":
    main()
