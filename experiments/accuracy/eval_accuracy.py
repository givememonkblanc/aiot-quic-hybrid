#!/usr/bin/env python3
"""
Score the on-device model against the reference detector.

Joins labels.csv (reference detector, see label_frames.py) with ai_recv.csv
(the device's per-frame AI objects) on gen_ts_ms, which both sides derive from
the same capture — so no host/device clock alignment is involved.

Reports, in order of what each answers:

  per-frame       how well does raw_score separate person from no-person?
                  PR/ROC over every threshold, plus the operating points that
                  matter: presence.enter, and the model's own det_threshold.
  transitions     how long after a person appears does the sensor say so?
                  Uses presence events from the receiver's mqtt_events.csv,
                  whose ts_ms is the device's esp_timer domain — the same clock
                  gen_ts_ms is stamped from.

Usage:
    python3 eval_accuracy.py --labels out/labels.csv --ai ai_recv.csv \
        --events ../../host_receiver/mqtt_events.csv --enter 50 --det-thr 150
"""

import argparse
import bisect
import csv
import json
import sys


def load_labels(path, ref_conf):
    """gen_ts_ms -> (ground truth bool, reference max confidence)."""
    out = {}
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            ts = int(row["gen_ts_ms"])
            conf = float(row["max_conf"])
            out[ts] = (conf >= ref_conf, conf)
    return out


def load_ai(path):
    """gen_ts_ms -> row dict of the device's AI object."""
    out = {}
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            out[int(row["gen_ts_ms"])] = {
                "raw_score": int(row["raw_score"]),
                "det_count": int(row["det_count"]),
                "source_seq": int(row["source_seq"]),
            }
    return out


def confusion(pairs, threshold):
    """(tp, fp, tn, fn) for 'device says person when raw_score >= threshold'."""
    tp = fp = tn = fn = 0
    for gt, score in pairs:
        pred = score >= threshold
        if gt and pred:
            tp += 1
        elif gt and not pred:
            fn += 1
        elif not gt and pred:
            fp += 1
        else:
            tn += 1
    return tp, fp, tn, fn


def prf(tp, fp, tn, fn):
    precision = tp / (tp + fp) if tp + fp else float("nan")
    recall = tp / (tp + fn) if tp + fn else float("nan")
    if precision == precision and recall == recall and (precision + recall) > 0:
        f1 = 2 * precision * recall / (precision + recall)
    else:
        f1 = float("nan")
    accuracy = (tp + tn) / (tp + fp + tn + fn) if (tp + fp + tn + fn) else float("nan")
    return precision, recall, f1, accuracy


def auc_roc(pairs):
    """Rank-based AUC (Mann-Whitney U). NaN when one class is absent."""
    pos = sorted(s for gt, s in pairs if gt)
    neg = sorted(s for gt, s in pairs if not gt)
    if not pos or not neg:
        return float("nan")
    total = 0.0
    for s in pos:
        lo = bisect.bisect_left(neg, s)
        hi = bisect.bisect_right(neg, s)
        total += lo + 0.5 * (hi - lo)   # ties count half
    return total / (len(pos) * len(neg))


def fmt(x, nd=3):
    return "n/a" if x != x else f"{x:.{nd}f}"


def report_operating_point(name, pairs, threshold):
    tp, fp, tn, fn = confusion(pairs, threshold)
    p, r, f1, acc = prf(tp, fp, tn, fn)
    print(f"  {name:<34} thr={threshold:>3}  "
          f"P={fmt(p)} R={fmt(r)} F1={fmt(f1)} acc={fmt(acc)}  "
          f"(tp={tp} fp={fp} tn={tn} fn={fn})")


def load_presence_events(path):
    """(ts_ms, state) from the receiver's MQTT log, device esp_timer domain."""
    events = []
    with open(path, newline="") as fh:
        # recv_us,cid,topic,payload_len,payload
        for row in csv.reader(fh):
            if len(row) < 5 or not row[2].endswith("/presence/event"):
                continue
            try:
                payload = json.loads(row[4])
                events.append((int(payload["ts_ms"]), payload["state"]))
            except (ValueError, KeyError):
                continue
    events.sort()
    return events


def reference_transitions(labels, debounce):
    """gen_ts of each absent->present flip in the reference, after debounce
    consecutive positive frames (mirrors the device's enter_frames)."""
    series = sorted(labels.items())
    transitions = []
    run = 0
    occupied = False
    for i, (ts, (gt, _conf)) in enumerate(series):
        if gt:
            run += 1
            if not occupied and run >= debounce:
                # credit the transition to the frame the run started on, not
                # the one that satisfied the debounce
                transitions.append(series[i - debounce + 1][0])
                occupied = True
        else:
            run = 0
            occupied = False
    return transitions


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", required=True)
    ap.add_argument("--ai", required=True)
    ap.add_argument("--events", help="host_receiver/mqtt_events.csv (optional)")
    ap.add_argument("--ref-conf", type=float, default=0.35,
                    help="reference confidence above which a frame counts as "
                         "containing a person (default 0.35)")
    ap.add_argument("--enter", type=int, default=50,
                    help="device presence.enter threshold to report (default 50)")
    ap.add_argument("--det-thr", type=int, default=150,
                    help="the model's own detection_threshold (default 150)")
    ap.add_argument("--debounce", type=int, default=2,
                    help="reference frames required to call a person present "
                         "(match presence.frames, default 2)")
    ap.add_argument("--sweep-out", help="write the full threshold sweep to CSV")
    args = ap.parse_args()

    labels = load_labels(args.labels, args.ref_conf)
    ai = load_ai(args.ai)

    common = sorted(set(labels) & set(ai))
    if not common:
        sys.exit("no frames in common — labels and ai_recv cover different runs")

    pairs = [(labels[ts][0], ai[ts]["raw_score"]) for ts in common]
    n_pos = sum(1 for gt, _ in pairs if gt)

    print(f"frames labelled     : {len(labels)}")
    print(f"AI objects received : {len(ai)}")
    print(f"joined on gen_ts_ms : {len(common)}"
          f"  (unmatched: {len(labels) - len(common)} frames, "
          f"{len(ai) - len(common)} AI objects)")
    print(f"reference positives : {n_pos}/{len(common)} "
          f"({100.0 * n_pos / len(common):.1f}%) at ref_conf>={args.ref_conf}")
    print()

    if n_pos == 0 or n_pos == len(common):
        print("WARNING: the reference labels one class only — this capture "
              "cannot measure discrimination. Record a session with both an "
              "occupied and an empty stretch.")
        print()

    print(f"separation          : AUC={fmt(auc_roc(pairs), 4)}")
    print()
    print("operating points")
    report_operating_point("presence.enter (device gate)", pairs, args.enter)
    report_operating_point("model detection_threshold", pairs, args.det_thr)

    best = None
    sweep = []
    for thr in range(0, 256):
        tp, fp, tn, fn = confusion(pairs, thr)
        p, r, f1, acc = prf(tp, fp, tn, fn)
        sweep.append((thr, tp, fp, tn, fn, p, r, f1, acc))
        if f1 == f1 and (best is None or f1 > best[1]):
            best = (thr, f1)
    if best:
        report_operating_point("best F1 on this capture", pairs, best[0])

    if args.sweep_out:
        with open(args.sweep_out, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["threshold", "tp", "fp", "tn", "fn",
                        "precision", "recall", "f1", "accuracy"])
            for row in sweep:
                w.writerow([row[0], row[1], row[2], row[3], row[4]] +
                           [("" if v != v else round(v, 6)) for v in row[5:]])
        print(f"\nthreshold sweep -> {args.sweep_out}")

    if args.events:
        print()
        print("transitions (reference person-appears -> device says present)")
        events = load_presence_events(args.events)
        present_events = [ts for ts, state in events if state == "present"]
        refs = reference_transitions(labels, args.debounce)
        if not refs:
            print("  no absent->present flip in the reference on this capture")
        elif not present_events:
            print("  device published no presence events on this capture")
        else:
            latencies = []
            for ref_ts in refs:
                later = [t for t in present_events if t >= ref_ts]
                if later:
                    latencies.append(later[0] - ref_ts)
            if latencies:
                latencies.sort()
                mid = latencies[len(latencies) // 2]
                print(f"  matched {len(latencies)}/{len(refs)} transitions; "
                      f"latency min={latencies[0]}ms median={mid}ms "
                      f"max={latencies[-1]}ms")
            else:
                print(f"  {len(refs)} reference transitions, none followed by a "
                      f"device presence event — all missed")


if __name__ == "__main__":
    main()
