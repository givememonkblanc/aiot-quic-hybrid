#!/usr/bin/env python3
"""Validate bounded codec ablation; all ratios are delivered-result coverage."""

import argparse, csv, hashlib, json
from pathlib import Path
from analyze_remote import summarize
from analyze_evidence import coverage


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fmt(value, digits=3):
    return f"{value:.{digits}f}" if value is not None else "n/a"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("suite", type=Path)
    p.add_argument("--plot", action="store_true")
    a = p.parse_args()
    manifest = json.loads((a.suite / "manifest.json").read_text())
    rows = []
    invalid = []
    waiting = []
    hashes = {}
    sources = []
    for cell in manifest["results"]:
        if cell["status"] == "running":
            waiting.append(cell["run"])
            continue
        run = a.suite.parent / cell["run"]
        try:
            trial = json.loads((run / "manifest.json").read_text())
            if trial["status"] != "complete" or not trial.get("restore_verified"):
                raise ValueError("trial/restore incomplete")
            fw = manifest["firmware"][cell["policy"]]
            if trial["test_release"]["image_sha256"] != fw["sha256"]:
                raise ValueError("firmware hash mismatch")
            provenance = json.loads((Path(fw["path"]).parent / "provenance.json").read_text())
            if provenance["firmware_sha256"] != fw["sha256"]:
                raise ValueError("provenance mismatch")
            sources.append(provenance["source_hashes"])
            s = summarize(run)
            t = s["sender_totals"]["1"]
            events = [
                json.loads(line)
                for line in (run / "events.jsonl").read_text().splitlines()
                if line.strip()
            ]
            c = coverage(events, s["sender_totals"])
            level = cell["policy"].split("_")[1]
            expected = (400, 300) if level == "low" else (800, 600)
            seen = set()
            detection_signatures = set()
            for e in events:
                if e["event"] != "object":
                    continue
                if e["alias"] == 1:
                    if (e["width"], e["height"]) != expected:
                        raise ValueError("wrong image dimensions")
                    raw = run / f"frame-{e['group']:06d}.jpg"
                    if sha(raw) != e["sha256"] or raw.stat().st_size != e["bytes"]:
                        raise ValueError("image archive mismatch")
                    seen.add(e["sha256"])
                else:
                    wire = bytes.fromhex(e["result"]["result_hex"])
                    if wire[:2] != b"\xd1\x02" or len(wire) != 20 + 6 * wire[3]:
                        raise ValueError("AI payload shape")
                    detection_signatures.add(hashlib.sha256(wire[20:]).hexdigest())
            hashes.setdefault(level, set()).update(seen)
            calls = t["codec_calls"]
            n = t["source_count"]
            span = t["source_span_us"]
            r = dict(
                run=cell["run"],
                policy=cell["policy"],
                pipeline=provenance["configuration"].get("AIOT_MOQT_CODEC_PIPELINE:BOOL") == "ON",
                repeat=cell["repeat"],
                ai=c["delivered_ai_results"],
                evidence=c["linked_complete_pairs"],
                missing=c["missing_evidence"],
                coverage=c["coverage_of_delivered_results"],
                pairs_confirmed_on_time_lower=c["pair_ack_on_time_lower_bound"],
                orphan_images=len(c["orphan_images"]),
                ai_ack_p95_s=s["ai"]["source_to_ack_p95_s"],
                ai_ack_samples=s["ai"]["source_to_ack_samples"],
                image_goodput_kbps=s["preview"]["payload_goodput_bps"] / 1000,
                codec_calls=calls,
                codec_failures=t["codec_failures"],
                codec_wall_mean_ms=t["codec_wall_sum_us"] / calls / 1000 if calls else None,
                codec_decode_mean_ms=t["codec_decode_us"] / calls / 1000 if calls else None,
                codec_encode_mean_ms=t["codec_encode_us"] / calls / 1000 if calls else None,
                codec_cpu_mean_ms=t["codec_cpu_us"] / calls / 1000 if calls else None,
                pump_gap_max_ms=t["pump_gap_max_us"] / 1000,
                source_count=n,
                source_span_s=span / 1e6,
                source_rate_hz=(n - 1) * 1e6 / span if n > 1 and span else None,
                source_failed=t["source_failed"],
                source_gap_max_ms=t["source_max_gap_us"] / 1000,
                min_internal=t["min_internal"],
                min_psram=t["min_psram"],
                codec_scratch_max=t["codec_scratch_max"],
                image_signatures=sorted(seen),
                ai_detection_signatures=sorted(detection_signatures),
            )
            rows.append(r)
        except (ValueError, KeyError, OSError) as exc:
            invalid.append(dict(run=cell["run"], reason=str(exc)))
    controls = dict(
        source_code_identical=all(x == sources[0] for x in sources) if sources else None,
        images_identical_within_each_resolution={
            k: len(v) == 1 if v else None for k, v in hashes.items()
        },
        ai_detection_outputs_identical=len({v for r in rows for v in r["ai_detection_signatures"]})
        == 1
        if rows
        else None,
    )
    report = dict(
        scope="codec scheduling screening, not physical mission DESR",
        suite_status=manifest["status"],
        valid_runs=len(rows),
        invalid=invalid,
        pending=waiting,
        controls=controls,
        rows=rows,
    )
    (a.suite / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    if rows:
        with (a.suite / "comparison.csv").open("w") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    text = [
        "# Fixed-input codec scheduling screening",
        "",
        f"Completed valid runs: {len(rows)} / {len(manifest['cells'])}. Invalid: {len(invalid)}.",
        "",
        "| Policy | Evidence / received AI | Confirmed pairs | AI ACK p95 | Codec wall / task CPU | Max pump gap | Source Hz / max gap |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for r in sorted(rows, key=lambda r: r["policy"]):
        text.append(
            f"| {r['policy']}{'+overlap' if r['pipeline'] else ''} r{r['repeat']} | {r['evidence']} / {r['ai']} | {r['pairs_confirmed_on_time_lower']} | {fmt(r['ai_ack_p95_s'])} s | {fmt(r['codec_wall_mean_ms'], 0)} / {fmt(r['codec_cpu_mean_ms'], 0)} ms | {r['pump_gap_max_ms']:.1f} ms | {fmt(r['source_rate_hz'])} / {r['source_gap_max_ms']:.1f} ms |"
        )
    text += [
        "",
        "Each row is one run; this small screening block is descriptive, not a significance test. Source cadence is replay release, not live capture.",
        "Ratios are conditional on delivered AI results, not physical event recall. Images differ in resolution between low and medium.",
        "CPU is sampled FreeRTOS task accounting; pump gap includes all work between calls. No cycle-level attribution is made.",
        f"Controls: `{json.dumps(controls)}`",
        f"Invalid runs retained: `{json.dumps(invalid)}`",
        "",
    ]
    (a.suite / "RESULTS.md").write_text("\n".join(text))
    if a.plot and rows:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        rs = sorted(rows, key=lambda r: r["policy"])
        labels = [
            ("overlap_medium" if r["pipeline"] else r["policy"]).replace("_", "\n")
            + f"\nr{r['repeat']}"
            for r in rs
        ]
        fig, axes = plt.subplots(1, 3, figsize=(10, 3.5))
        for ax, key, title in zip(
            axes,
            ["coverage", "ai_ack_p95_s", "pump_gap_max_ms"],
            ["Evidence / delivered AI", "AI source→ACK p95 (s)", "Maximum QUIC pump gap (ms)"],
        ):
            ax.bar(labels, [r[key] for r in rs])
            ax.set_title(title, fontsize=10)
            ax.grid(axis="y", alpha=0.2)
        axes[0].set_ylim(0, 1)
        axes[2].set_yscale("log")
        fig.suptitle(
            "Same-input codec screening, 128 kbit/s — per-run outcomes; no physical GT", fontsize=11
        )
        fig.tight_layout()
        fig.savefig(a.suite / "comparison.pdf")
        fig.savefig(a.suite / "comparison.png", dpi=160)
        plt.close(fig)
    print(json.dumps({k: report[k] for k in ("valid_runs", "invalid", "pending", "controls")}))


if __name__ == "__main__":
    main()
