#!/usr/bin/env python3
"""Mission utility of device runs: count, evidence and history.

  C  share of the run in which the receiver knows the count of the device
  E  mean over the events of the quality of the best evidence that arrived before the
     evidence deadline, counted from the capture of the source image
  H  share of the results that arrived
The capture time of an event is known in the clock of the device. It is mapped to the clock
of the host through the device timestamps in the serial record: the smallest difference
between the host time of a line and the device time it reports is the offset.

The alert delay of a result that reports a passage runs from its production on the device to
its arrival at the receiver. The production is the end of the inference, start_us plus
duration_us of the device, mapped to the host clock by the lower envelope of the host time
minus the device time of the timestamped lines. The host
time of a serial line is not a production time: the device sends its lines in batches, and a
line reaches the host 40 ms after it was written at the median and 110 ms at the 90th
percentile. That time is kept as event_delay_netlog_s, which understates the delay."""

import argparse
import json
import statistics
import time
from pathlib import Path

from analyze_mission_device import serial_records
from count_metrics import incorrect_time

REPRESENTATION, TOTAL, NEW = "126", "14336", "14338"


def clock_map(stamps):
    """Device seconds -> host wall seconds: the smallest host-minus-device difference of the
    timestamped lines, about 900 in a run. The two clocks keep together: a line fitted to the
    smallest difference of each 30 s of a run has a slope of 1 ppm at the median over the 108
    runs of the alert campaign, and its spread from run to run, -22 to +48 ppm between the 10th
    and the 90th percentile, is that of the fit: no drift is applied."""
    base = min(t - d for d, t in stamps)

    def to_host(d):
        return d + base
    return to_host


def analyze(run, clock_offset, qualities, deadline_s, weights):
    manifest = json.loads((run / "manifest.json").read_text())
    # The events are in monotonic time and the serial record in wall time. The difference is
    # recorded with the run (clock_offset_s) or, for the runs before that, in clock.json; the
    # difference at analysis time holds only while the host has not restarted since the run.
    clock = run / "clock.json"
    if "clock_offset_s" in manifest:
        clock_offset = manifest["clock_offset_s"]
    elif clock.exists():
        clock_offset = json.loads(clock.read_text())["clock_offset_s"]
    events = [json.loads(x) for x in (run / "events.jsonl").read_text().splitlines() if x.strip()]
    epoch = max([e["t"] for e in events if e["event"] == "subscribed"][:2])
    produced, passages, offsets, mus, sizes, sent = {}, {}, [], None, {}, []
    seen, starts, durations, stamps = {}, {}, {}, []
    for t, tag, data in serial_records(run / "serial.log"):
        if tag == "MOQ_AI_PRODUCED":
            produced[data["seq"]] = dict(t=t - clock_offset - epoch, source_ms=data["source_ms"])
        elif tag == "MOQ_PASSAGE":
            passages[data["seq"]] = passages.get(data["seq"], 0) + 1
            seen[data["seq"]] = t
        elif tag == "MOQ_RESOURCE":
            offsets.append(t - data["ts_us"] / 1e6)
            stamps.append((data["ts_us"] / 1e6, t))
        elif tag == "AI_START":
            starts[data["seq"]] = data["start_us"] / 1e6
            stamps.append((data["start_us"] / 1e6, t))
        elif tag == "MOQ_CAPTURE":
            stamps.append((data["done_us"] / 1e6, t))
        elif tag == "AI_PROFILE":
            durations[data["seq"]] = data["duration_us"] / 1e6
        elif tag == "MOQ_MUS":
            mus = data
        elif tag == "MOQ_REPRESENTATION":
            sizes.setdefault(data["level"], []).append(data["bytes"])
            if data.get("admission") == 0:
                sent.append(dict(group=data["group"], level=data["level"], bytes=data["bytes"], start=t - clock_offset - epoch))
    # A line of the serial record may be lost. A result whose passage was recorded and whose
    # production was not is restored with the usual distance between capture and record.
    usual = statistics.median((p["t"] + clock_offset + epoch) - p["source_ms"] / 1000 for p in produced.values())
    restored = [seq for seq in seen if seq not in produced]
    for seq in restored:
        produced[seq] = dict(t=seen[seq] - clock_offset - epoch, source_ms=round(1000 * (seen[seq] - usual)))
    device_to_host = min(offsets) - clock_offset - epoch     # device seconds -> seconds after the subscription
    to_host = clock_map(stamps)
    total, truth = 0, []
    for seq in sorted(produced):
        total += passages.get(seq, 0)
        produced[seq].update(new=passages.get(seq, 0), total=total)
        truth.append((produced[seq]["t"], total))
    results, known, c = {}, [], 0
    for e in sorted((e for e in events if e["event"] == "object" and e.get("alias") == 2), key=lambda e: e["t"]):
        if e["group"] in produced and e["group"] not in results:
            results[e["group"]] = e["t"] - epoch
            c = max(c, e["properties"][TOTAL])
            known.append((e["t"] - epoch, c))
    best, arrivals, level_of = {}, [], {}
    for e in events:
        if e["event"] not in ("object", "fetch_object") or e.get("alias") != 3 or e["group"] not in produced:
            continue
        level = e.get("properties", {}).get(REPRESENTATION)
        if level is None:
            continue
        capture = produced[e["group"]]["source_ms"] / 1000 + device_to_host
        age = e["t"] - epoch - capture
        quality = qualities[2 - level]
        arrivals.append(dict(group=e["group"], level=level, bytes=e["bytes"], age_s=age, in_time=age <= deadline_s,
                             t=e["t"] - epoch))
        if age <= deadline_s and quality > best.get(e["group"], 0.0):
            best[e["group"]] = quality
            level_of[e["group"]] = level
    event_groups = [g for g in produced if produced[g]["new"]]
    span = max([p["t"] for p in produced.values()] + list(results.values()) + [0.0])
    C = 1.0 - incorrect_time(truth, known, 0.0, span) / span if span else 0.0
    E = statistics.mean(best.get(g, 0.0) for g in event_groups) if event_groups else 1.0
    H = len(results) / len(produced) if produced else 0.0
    resets = [e for e in events if e["event"] == "reset"]
    # every evidence object that entered the transport: from its hand-over to its arrival or its reset
    objects = []
    for o in sent:
        if o["group"] not in produced:
            continue
        end = [a for a in arrivals if a["group"] == o["group"] and a["level"] == o["level"] and a["t"] >= o["start"]]
        gone = [e["t"] - epoch for e in resets if e.get("group") == o["group"] and e.get("alias") == 3 and e["t"] - epoch >= o["start"]]
        o.update(capture=produced[o["group"]]["source_ms"] / 1000 + device_to_host, known=produced[o["group"]]["t"])
        if end:
            o.update(end=end[0]["t"], outcome="in time" if end[0]["in_time"] else "late")
        elif gone:
            o.update(end=min(gone), outcome="reset")
        else:
            o.update(end=None, outcome="unfinished")
        objects.append(o)
    best_level = {str(k): sum(1 for g in event_groups if level_of.get(g) == k) for k in (0, 1, 2)}
    best_level["none"] = sum(1 for g in event_groups if g not in level_of)
    image_bytes = sum(a["bytes"] for a in arrivals)
    wc, we, wh = weights
    return dict(
        run=run.name, status=manifest["status"], network=manifest["network"], rate_trace=manifest.get("rate_trace", []),
        firmware=manifest["test_release"]["elf_sha256"], results=len(produced), events=len(event_groups),
        C=C, E=E, H=H, U=wc * C + we * E + wh * H,
        evidence_in_time=sum(1 for g in event_groups if g in best) / len(event_groups) if event_groups else 1.0,
        evidence_by_level={str(k): sum(1 for a in arrivals if a["level"] == k and a["in_time"]) for k in (0, 1, 2)},
        evidence_late=sum(1 for a in arrivals if not a["in_time"]), evidence_age_s=sorted(a["age_s"] for a in arrivals),
        restored_results=len(restored), evidence_best=best_level, objects=objects,
        event_times=[dict(group=g, known=produced[g]["t"], capture=produced[g]["source_ms"] / 1000 + device_to_host)
                     for g in event_groups],
        result_delay_s=sorted(results[g] - produced[g]["t"] for g in results),
        event_delay_s=sorted(results[g] - (to_host(starts[g] + durations[g]) - clock_offset - epoch)
                             for g in event_groups if g in results and g in starts and g in durations),
        # [production, arrival] of each alert, seconds after the subscription; arrival None if lost
        event_alerts=[[to_host(starts[g] + durations[g]) - clock_offset - epoch, results.get(g)]
                      for g in event_groups if g in starts and g in durations],
        event_alert_groups=[g for g in event_groups if g in starts and g in durations],
        event_delay_netlog_s=sorted(results[g] - produced[g]["t"] for g in event_groups if g in results),
        event_lost=sum(1 for g in event_groups if g not in results),
        image_bytes_delivered=image_bytes, image_bytes_late=sum(a["bytes"] for a in arrivals if not a["in_time"]),
        stream_resets=len(resets), mus=mus,
        count_error=total - (known[-1][1] if known else 0))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("runs", nargs="+", type=Path)
    p.add_argument("--out", type=Path)
    p.add_argument("--qualities", type=float, nargs=3, default=[0.115, 0.491, 1.0], help="low, medium, high")
    p.add_argument("--deadline", type=float, default=10.0)
    p.add_argument("--weights", type=float, nargs=3, default=[0.4, 0.5, 0.1])
    a = p.parse_args()
    offset = time.time() - time.monotonic()
    reports = []
    for run in a.runs:
        try:
            reports.append(analyze(run, offset, a.qualities, a.deadline, a.weights))
        except (OSError, ValueError, KeyError, IndexError) as failure:
            print(f"{run.name}: unusable, {failure!r}")
    for r in reports:
        print(f"{r['run']:44s} {r['status']:9s} U {r['U']:.3f}  C {r['C']:.3f}  E {r['E']:.3f}  H {r['H']:.3f}  "
              f"in time {r['evidence_in_time']:.2f}  levels {r['evidence_by_level']}  late {r['evidence_late']}  "
              f"resets {r['stream_resets']}")
    if a.out:
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(json.dumps(dict(schema=1, qualities=a.qualities, deadline_s=a.deadline, weights=a.weights,
                                         reports=reports), indent=1) + "\n")


if __name__ == "__main__":
    main()
