#!/usr/bin/env python3
"""Per-phase analysis of scenario-mode runs (APP_SCENARIO_MODE).

Reads each run directory's monitor.log for the [SCENARIO] phase markers
(persisted to phases.csv, since raw logs are gitignored) and splits the
tx_events / shed events by phase: 0 = normal, 1 = image burst, 2 = recovery.
Phase timestamps (t_ms) and tx_events log_time_ms are both milliseconds
since device boot, so they align directly.

    python3 analyze_scenario.py runs_v2/hybrid_scenario_r1 [more dirs...]

Writes scenario_summary.json + phases.csv into each run dir and prints an
aggregate per-phase table across all given dirs (grouped by label prefix
before the _rN suffix).
"""
from __future__ import annotations

import csv
import json
import math
import re
import sys
from collections import defaultdict
from pathlib import Path
from statistics import mean

PHASE_RE = re.compile(r"\[SCENARIO\] phase=(\d+) elapsed_s=(\d+) t_ms=(\d+)")
IMAGE_TX_RE = re.compile(r"\[SCENARIO\] image_tx len=(\d+) chunks=(\d+) t_ms=(\d+)")
SHED_RE = re.compile(r"\[(EXPIRED|EVICTED)\]\s+track=(\d+)\s+seq=(\d+)\s+age_us=(-?\d+)")
LOG_TIME_RE = re.compile(r"[A-Z] \((\d+)\)")
PHASE_NAMES = {0: "normal", 1: "burst", 2: "recovery"}


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct / 100.0
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return ordered[lower]
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def parse_phases(monitor_log: Path) -> tuple[list[dict], list[dict], list[dict]]:
    phases, image_tx, sheds = [], [], []
    with monitor_log.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            m = PHASE_RE.search(line)
            if m:
                phases.append({"phase": int(m.group(1)), "elapsed_s": int(m.group(2)), "t_ms": int(m.group(3))})
                continue
            m = IMAGE_TX_RE.search(line)
            if m:
                image_tx.append({"len": int(m.group(1)), "chunks": int(m.group(2)), "t_ms": int(m.group(3))})
                continue
            m = SHED_RE.search(line)
            if m:
                t = LOG_TIME_RE.search(line)
                sheds.append({"kind": m.group(1), "track": int(m.group(2)),
                              "t_ms": int(t.group(1)) if t else 0})
    return phases, image_tx, sheds


def phase_of(t_ms: int, phases: list[dict]) -> int:
    current = -1
    for p in phases:
        if t_ms >= p["t_ms"]:
            current = p["phase"]
    return current


def analyze_run(run_dir: Path) -> dict | None:
    monitor_log = run_dir / "monitor.log"
    if not monitor_log.exists():
        print(f"skip {run_dir}: no monitor.log", file=sys.stderr)
        return None
    phases, image_tx, sheds = parse_phases(monitor_log)
    if not phases:
        print(f"skip {run_dir}: no [SCENARIO] markers", file=sys.stderr)
        return None

    with (run_dir / "phases.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["phase", "elapsed_s", "t_ms"])
        writer.writeheader()
        writer.writerows(phases)

    tx_by_phase: dict[int, list[dict]] = defaultdict(list)
    tx_path = run_dir / "tx_events.csv"
    if tx_path.exists():
        for row in csv.DictReader(tx_path.open()):
            t_ms = int(row["log_time_ms"] or 0)
            tx_by_phase[phase_of(t_ms, phases)].append(row)

    shed_by_phase: dict[int, int] = defaultdict(int)
    for shed in sheds:
        shed_by_phase[phase_of(shed["t_ms"], phases)] += 1

    result: dict = {"run": run_dir.name, "phases": {}}
    for phase in (0, 1, 2):
        rows = tx_by_phase.get(phase, [])
        e2es = [float(r["e2e_us"]) / 1000 for r in rows if float(r.get("e2e_us") or 0) > 0]
        qwaits = [float(r["qwait_us"]) / 1000 for r in rows if r.get("qwait_us")]
        result["phases"][PHASE_NAMES[phase]] = {
            "tx_events": len(rows),
            "tx_datagram": sum(1 for r in rows if r["mode"] == "datagram"),
            "tx_stream": sum(1 for r in rows if r["mode"] == "stream"),
            "e2e_ms_avg": round(mean(e2es), 2) if e2es else 0.0,
            "e2e_ms_p95": round(percentile(e2es, 95), 2) if e2es else 0.0,
            "qwait_ms_avg": round(mean(qwaits), 3) if qwaits else 0.0,
            "qwait_ms_p95": round(percentile(qwaits, 95), 3) if qwaits else 0.0,
            "shed": shed_by_phase.get(phase, 0),
        }
    result["image_frames_tx"] = len(image_tx)
    result["image_chunks_tx"] = sum(i["chunks"] for i in image_tx)
    result["shed_total"] = len(sheds)

    (run_dir / "scenario_summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(1)
    groups: dict[str, list[dict]] = defaultdict(list)
    for arg in sys.argv[1:]:
        res = analyze_run(Path(arg))
        if res:
            label = re.sub(r"_r\d+$", "", res["run"])
            groups[label].append(res)

    for label, runs in groups.items():
        print(f"\n[{label}] n={len(runs)} reps — per-phase mean")
        print(f"{'phase':10s} {'tx':>6s} {'dgram':>6s} {'stream':>7s} {'e2e_avg':>9s} {'e2e_p95':>9s} {'qwait_p95':>10s} {'shed':>6s}")
        for phase in ("normal", "burst", "recovery"):
            vals = [r["phases"][phase] for r in runs]
            print(f"{phase:10s} {mean(v['tx_events'] for v in vals):6.0f} "
                  f"{mean(v['tx_datagram'] for v in vals):6.0f} {mean(v['tx_stream'] for v in vals):7.0f} "
                  f"{mean(v['e2e_ms_avg'] for v in vals):7.1f}ms {mean(v['e2e_ms_p95'] for v in vals):7.1f}ms "
                  f"{mean(v['qwait_ms_p95'] for v in vals):8.2f}ms {mean(v['shed'] for v in vals):6.1f}")
        print(f"image frames/chunks per rep: {mean(r['image_frames_tx'] for r in runs):.0f}/{mean(r['image_chunks_tx'] for r in runs):.0f}")


if __name__ == "__main__":
    main()
