#!/usr/bin/env python3
"""Count-only bounds: count agreement cannot establish TP, FP or F1."""
import argparse
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("predictions", type=Path,
                   help="JSONL: frame, split, gt_count, pred_count")
    p.add_argument("--split", default="test")
    p.add_argument("--out", type=Path)
    a = p.parse_args()
    rows = [json.loads(x) for x in a.predictions.read_text().splitlines() if x.strip()]
    rows = [r for r in rows if r.get("split") == a.split]
    if not rows:
        raise SystemExit(f"no {a.split} predictions")
    gt = sum(int(r["gt_count"]) for r in rows)
    detected = sum(min(int(r["gt_count"]), int(r["pred_count"])) for r in rows)
    fp = sum(max(0, int(r["pred_count"])-int(r["gt_count"])) for r in rows)
    missed = gt-detected
    report = dict(schema=2, scope="count-only optimistic bounds; no spatial matching", split=a.split,
                  frames=len(rows), gt_screws=gt, maximum_possible_tp=detected,
                  minimum_missed=missed, minimum_false_positives=fp,
                  miss_rate_lower_bound=missed/gt if gt else None,
                  recall_upper_bound=detected/gt if gt else None,
                  precision_upper_bound=detected/(detected+fp) if detected+fp else None,
                  caveat="Equal GT/predicted counts may still contain all false positives. Exact precision, recall and F1 require position/class matching.")
    text = json.dumps(report, indent=2)
    if a.out:
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
