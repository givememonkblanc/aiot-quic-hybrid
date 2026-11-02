#!/usr/bin/env python3
"""Mission accounting of device trials: the serial record of the device against the receiver log.

The ledger is rebuilt from the serial output: a result exists once the device printed
[MOQ_AI_PRODUCED], and its passage events are the [MOQ_PASSAGE] lines with its sequence.
Lag is counted from the production of the result on the device to its arrival."""

import argparse
import json
import re
import time
from pathlib import Path

from evaluate_passages import evaluate

TAG = re.compile(r"^(\d+\.\d+) .*?\[([A-Z_0-9]+)\] ?(.*)$")


def serial_records(path):
    """The records of the boot that ran the trial. A record may begin with the last lines of the
    previous image, up to its notice of the update; those belong to another run."""
    records = []
    for line in path.read_text(errors="replace").splitlines():
        m = TAG.match(line)
        if not m:
            continue
        body = m.group(3).strip()
        try:
            data = json.loads(body) if body.startswith("{") else body
        except json.JSONDecodeError:
            data = body
        records.append((float(m.group(1)), m.group(2), data))
    update = [i for i, (_, tag, data) in enumerate(records)
              if tag == "MOQ_OTA" and data == "update_available=1"]
    if update and any(tag == "MOQ_BOOT" for _, tag, _ in records[update[-1]:]):
        records = records[update[-1] + 1:]
    # The notice of the update travels by UDP and may be lost; the boot of the trial then begins at
    # the NETLOG record that precedes its MOQ_BOOT.
    boots = [i for i, (_, tag, _) in enumerate(records) if tag == "MOQ_BOOT"]
    if boots:
        start = max((i for i, (_, tag, _) in enumerate(records[:boots[-1]]) if tag == "NETLOG"), default=0)
        records = records[start:]
    yield from records


def analyze(run, clock_offset):
    """`clock_offset`: wall clock minus monotonic clock of the host that recorded both logs."""
    manifest = json.loads((run / "manifest.json").read_text())
    events = [json.loads(x) for x in (run / "events.jsonl").read_text().splitlines() if x.strip()]
    records = list(serial_records(run / "serial.log"))
    produced, passages, device = {}, {}, dict(expired=0, reconnects=0, queue_overflow=0, resent=0)
    config, stats, internal = None, None, []
    for t, tag, data in records:
        if tag == "MOQ_AI_PRODUCED":
            # A result that found no room in the queue was produced all the same.
            produced[data["seq"]] = t - clock_offset
            if not data.get("enqueued"):
                device["queue_overflow"] += 1
        elif tag == "MOQ_PASSAGE":
            passages[data["seq"]] = passages.get(data["seq"], 0) + 1
        elif tag == "MOQ_EXPIRED":
            device["expired"] += 1
        elif tag == "MOQ_RESENT":
            device["resent"] += 1
        elif tag == "MOQ_RECONNECT":
            device["reconnects"] += 1
        elif tag == "MOQ_PASSAGE_CONFIG":
            config = data
        elif tag == "MOQ_POLICY_STATS":
            stats = data
        elif tag == "MOQ_RESOURCE":
            internal.append(data["internal_free"])
    ledger, total = [], 0
    for group in sorted(produced):
        total += passages.get(group, 0)
        ledger.append(dict(group=group, capture_t=produced[group], passages_new=passages.get(group, 0),
                           passages_total=total))
    subscribed = [e["t"] for e in events if e["event"] == "subscribed"]
    epoch = max(subscribed[:2])
    outages = [(epoch + o["start_s"], epoch + o["end_s"]) for o in manifest.get("outages", [])]
    report = evaluate(ledger, events, outages)
    finished = [e for e in events if e["event"] == "finished"]
    report.update(
        run=run.name,
        status=manifest["status"],
        firmware=manifest["test_release"]["elf_sha256"],
        network=manifest["network"],
        outages=manifest.get("outages", []),
        config=config,
        device=dict(device, min_internal_free=min(internal, default=None),
                    **{k: stats[k] for k in ("ai_timeout_resets", "image_timeout_resets", "retained_only",
                                             "fetch_objects", "cache_evicted") if stats and k in stats}),
        receiver=dict(
            sessions=sum(e["event"] == "setup" for e in events),
            finished=bool(finished),
            reported_total=finished[-1]["passages_total"] if finished else None,
            lost=len([e for e in events if e["event"] == "lost"]),
            errors=[e.get("reason") for e in events if e["event"] == "error"],
        ),
    )
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("runs", nargs="+", type=Path)
    p.add_argument("--out", type=Path)
    a = p.parse_args()
    offset = time.time() - time.monotonic()
    reports = []
    for run in a.runs:
        try:
            reports.append(analyze(run, offset))
        except (OSError, ValueError, KeyError, IndexError) as failure:
            reports.append(dict(run=run.name, status="unusable", reason=repr(failure)))
    rows = ["| 시험 | 상태 | 결과 도착/생성 | 통과 사건 도착/생성 | 개수 오차 | 단절 중 생성→도착 | 지연 p95 (s) | 지연 최대 (s) "
            "| 복구 | 시한 폐기 | 재접속 | 이미지 |", "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in reports:
        if "results_produced" not in r:
            rows.append(f"| {r['run']} | {r['status']}: {r.get('reason', '')} |||||||||||")
            continue
        lag = {k: (float("nan") if v is None else v) for k, v in r["result_lag_s"].items()}
        rows.append(
            f"| {r['run']} | {r['status']} | {r['results_received']}/{r['results_produced']} "
            f"| {r['passage_events_received']}/{r['passage_events_produced']} | {r['count_error']} "
            f"| {r['received_of_outage']}/{r['produced_during_outage']} | {lag['p95']:.2f} | {lag['max']:.2f} "
            f"| {r['results_recovered']} | {r['device']['expired'] + r['device'].get('ai_timeout_resets', 0)} "
            f"| {r['device']['reconnects']} | {r['images_received']} |")
    text = "\n".join(rows) + "\n"
    print(text)
    if a.out:
        a.out.mkdir(parents=True, exist_ok=True)
        (a.out / "summary.json").write_text(json.dumps(dict(schema=1, kind="device", reports=reports), indent=2) + "\n")
        (a.out / "summary.md").write_text(text)


if __name__ == "__main__":
    main()
