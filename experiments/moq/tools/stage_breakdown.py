#!/usr/bin/env python3
"""Where the time of a frame goes, per object kind, from the logs of a run (no new instrumentation).

Clocks: the camera's events in its own clock (esp_timer); the relay (relay.jsonl) and the operator's subscriber
(sub-remote/events.jsonl) share the laptop's monotonic clock; the helper has its own. The camera clock is mapped to
the laptop's by the smallest (relay receipt - camera admission) over the run's objects, the helper clock by the
smallest (relay receipt of its result - its inference end): each mapped stage is the excess over the fastest
object, so the camera->relay and helper->relay stages are biased low by one fixed one-way delay (a few ms).

Stages (s), per kind:
  result (camera's own inference, track 2): wait (capture -> the detector takes the frame), infer, publish
    (result -> queued for sending), uplink (queued -> at the relay: send queue + radio), operator (relay -> operator)
  split (helper task, track 1 then helper result track 5): wait, decode, encode (decode end -> queued),
    uplink, helper (at the relay -> its result back at the relay; helper_infer inside), operator
  evidence (track 3): prepare (capture -> queued: planner wait + conversion), uplink, operator
usage: stage_breakdown.py RUN... -> pooled per method and situation (p50 / p90)"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

LINE = re.compile(r"(\S+) \[([A-Z_]+)\] (\{.*\})")


def load(run):
    ser = defaultdict(list)
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        m = LINE.match(line)
        if m:
            try:
                ser[m.group(2)].append(json.loads(m.group(3)))
            except ValueError:
                pass
    relay_q = {}
    rp = run / "relay.jsonl"
    for line in (rp.open() if rp.exists() else []):
        try:
            r = json.loads(line)
        except ValueError:
            continue
        if r.get("ev") == "queued":
            k = (r["alias"], r["group"])
            relay_q[k] = min(relay_q.get(k, 1e18), r["t"])
    sub = {}
    for line in (run / "sub-remote" / "events.jsonl").open():
        r = json.loads(line)
        if r.get("event") in ("object", "fetch_object") and "alias" in r:
            k = (r["alias"], r["group"])
            sub[k] = min(sub.get(k, 1e18), r["t"])
    helper = {}
    hp = run / "helper.log"
    for line in (hp.read_text(errors="replace").splitlines() if hp.exists() else []):
        if "[HELPER_DET]" in line:
            j = json.loads(line.split(" ", 2)[2])
            helper.setdefault(j["group"], j)
    return ser, relay_q, sub, helper


def stages(run):
    ser, rq, sub, helper = load(run)
    adm = {(a["alias"], a["group"]): a["admitted_us"] / 1e6 for a in ser["MOQ_ADMITTED"]}
    off = min((rq[k] - t for k, t in adm.items() if k in rq), default=None)   # camera -> laptop clock
    start = {a["seq"]: a for a in ser["AI_START"]}
    out = defaultdict(lambda: defaultdict(list))
    if off is None:   # no relay log of the camera's objects (MQTT): uplink and relay stages merged into operator
        off = min((sub[k] - t for k, t in adm.items() if k in sub), default=0.0)
    hoff = min((rq[(5, g)] - h["end_us"] / 1e6 for g, h in helper.items() if (5, g) in rq), default=None)
    for p in ser["AI_PROFILE"]:
        s = start.get(p["seq"])
        k = (2, p["seq"])
        if not s or k not in adm:
            continue
        cap, st = s["capture_ms"] / 1e3, s["start_us"] / 1e6
        o = out["result"]
        o["wait"].append(st - cap)
        o["infer"].append(p["duration_us"] / 1e6)
        o["publish"].append(adm[k] - st - p["duration_us"] / 1e6)
        if k in rq:
            o["uplink"].append(rq[k] - (adm[k] + off))
            if k in sub:
                o["operator"].append(sub[k] - rq[k])
        if k in sub:
            o["total"].append(sub[k] - (cap + off))
    for sp in ser["AI_SPLIT"]:
        s = start.get(sp["seq"])
        k = (1, sp["seq"])
        if not s or k not in adm:
            continue
        cap, st = s["capture_ms"] / 1e3, s["start_us"] / 1e6
        o = out["split"]
        o["wait"].append(st - cap)
        o["decode"].append(sp["decode_us"] / 1e6)
        o["encode"].append(adm[k] - st - sp["decode_us"] / 1e6)
        r5 = (5, sp["seq"])
        if k in rq:
            o["uplink"].append(rq[k] - (adm[k] + off))
            if r5 in rq:
                o["helper"].append(rq[r5] - rq[k])
        h = helper.get(sp["seq"])
        if h:
            o["helper_infer"].append((h["end_us"] - h["start_us"]) / 1e6)
        if r5 in rq and r5 in sub:
            o["operator"].append(sub[r5] - rq[r5])
        if r5 in sub:
            o["total"].append(sub[r5] - (cap + off))
    seen = set()
    for ob in ser["MOQ_OBJECT"]:
        k = (3, ob["group"])
        if k in seen or k not in adm:
            continue
        seen.add(k)
        o = out["evidence"]
        cap = ob["capture_us"] / 1e6
        o["prepare"].append(adm[k] - cap)
        if k in rq:
            o["uplink"].append(rq[k] - (adm[k] + off))
            if k in sub:
                o["operator"].append(sub[k] - rq[k])
        if k in sub:
            o["total"].append(sub[k] - (cap + off))
    return out


def main():
    pool = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for p in sys.argv[1:]:
        run = Path(p)
        m = re.match(r"\w+-(\w+)-(.+)-r\d$", run.name)
        if not m:
            continue
        for kind, st in stages(run).items():
            for name, v in st.items():
                pool[m.groups()][kind][name] += v
    order = {"result": ["wait", "infer", "publish", "uplink", "operator", "total"],
             "split": ["wait", "decode", "encode", "uplink", "helper", "helper_infer", "operator", "total"],
             "evidence": ["prepare", "uplink", "operator", "total"]}
    for key in sorted(pool):
        print(f"== {key[1]} @ {key[0]}")
        for kind in ("result", "split", "evidence"):
            st = pool[key].get(kind)
            if not st:
                continue
            cells = [f"{n} {np.median(st[n]):.2f}/{np.percentile(st[n], 90):.2f}" for n in order[kind] if st.get(n)]
            print(f"  {kind:8s} n={len(st[order[kind][0]]):4d} | " + " | ".join(cells))


if __name__ == "__main__":
    main()
