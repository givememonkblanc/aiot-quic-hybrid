#!/usr/bin/env python3
"""Analyze the receiver-side datagram log written by picoquicdemo (mqtt ALPN).

host_receiver/picoquic writes dgram_recv.csv (override: AIOT_DGRAM_LOG env)
with one row per received QUIC DATAGRAM:

    recv_us,cid,valid,track_id,priority,flags,object_seq,gen_ts_ms,payload_len

Per connection (cid) and per track this script derives what the sender-side
telemetry cannot see:

  * received count and gap-based loss: (max_seq - min_seq + 1) - unique_seqs
  * duplicates and reordering (arrival with object_seq <= previous max)
  * one-way delay variation (OWDV) as an age-of-information proxy: the device
    clock (gen_ts_ms, esp_timer domain) and the server clock are unsynchronized,
    so both are normalized to the connection's first sample and the difference
    delta(recv) - delta(gen) is reported (avg/p50/p95/p99, ms). A rising OWDV
    trend indicates queue buildup; shed/expired objects appear as seq gaps.

    python3 parse_receiver_dgram_log.py dgram_recv.csv --out-dir runs_v2/hybrid_r1
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean

TRACK_NAMES = {1: "telemetry", 2: "ai_result", 3: "image"}


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


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def analyze_track(rows: list[dict[str, str]]) -> dict:
    """rows: arrival-ordered valid rows of one (cid, track_id) group."""
    seqs = [int(r["object_seq"]) for r in rows]
    unique = set(seqs)
    lo, hi = min(unique), max(unique)
    expected = hi - lo + 1

    duplicates = len(seqs) - len(unique)
    reordered = 0
    max_seen = -1
    for seq in seqs:
        if seq <= max_seen:
            reordered += 1
        else:
            max_seen = seq

    base_recv = int(rows[0]["recv_us"])
    base_gen = int(rows[0]["gen_ts_ms"])
    owdv_ms = [
        (int(r["recv_us"]) - base_recv) / 1000.0 - (int(r["gen_ts_ms"]) - base_gen)
        for r in rows
    ]

    inter_ms = [
        (int(b["recv_us"]) - int(a["recv_us"])) / 1000.0
        for a, b in zip(rows, rows[1:])
    ]

    return {
        "received": len(seqs),
        "unique": len(unique),
        "seq_min": lo,
        "seq_max": hi,
        "lost_gap": expected - len(unique),
        "loss_pct": round(100.0 * (expected - len(unique)) / expected, 3) if expected else 0.0,
        "duplicates": duplicates,
        "reordered": reordered,
        "payload_bytes": sum(int(r["payload_len"]) for r in rows),
        "owdv_ms_avg": round(mean(owdv_ms), 3),
        "owdv_ms_p50": round(percentile(owdv_ms, 50), 3),
        "owdv_ms_p95": round(percentile(owdv_ms, 95), 3),
        "owdv_ms_p99": round(percentile(owdv_ms, 99), 3),
        "interarrival_ms_avg": round(mean(inter_ms), 3) if inter_ms else 0.0,
        "interarrival_ms_p95": round(percentile(inter_ms, 95), 3) if inter_ms else 0.0,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path, help="dgram_recv.csv from picoquicdemo")
    parser.add_argument("--out-dir", type=Path, default=None,
                        help="write receiver_dgram_summary.json here (default: print only)")
    args = parser.parse_args()

    rows = load_rows(args.log)
    groups: dict[tuple[str, int], list[dict[str, str]]] = defaultdict(list)
    invalid = 0
    for row in rows:
        if row.get("valid") == "1":
            groups[(row["cid"], int(row["track_id"]))].append(row)
        else:
            invalid += 1

    summary: dict = {"log": str(args.log), "total_rows": len(rows), "invalid_rows": invalid, "connections": {}}
    for (cid, track_id), track_rows in sorted(groups.items()):
        track_name = TRACK_NAMES.get(track_id, f"track{track_id}")
        summary["connections"].setdefault(cid, {})[track_name] = analyze_track(track_rows)

    print(json.dumps(summary, indent=2))
    if args.out_dir is not None:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        out_path = args.out_dir / "receiver_dgram_summary.json"
        out_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(f"\nwrote {out_path}")


if __name__ == "__main__":
    main()
