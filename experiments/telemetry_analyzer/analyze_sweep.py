#!/usr/bin/env python3
"""Summarize the netem RTT/loss sweep: per transport-mode x condition metrics.

Reads experiments/runs_v2/sweep_<mode>_<cond>_r*/ and tabulates how each mode
degrades under RTT and loss (paper eval, Saif&Matrawy-style sweep). Sources:
  telemetry.csv               rtt_ms, datagrams_lost, expired/evicted, stream_send_fail
  tx_events.csv               e2e_us (freshness), qwait_us (queue wait)
  receiver_dgram_summary.json connections[*].telemetry.loss_pct / owdv_ms_p95 (AoI)
"""
import csv, glob, json, os, statistics as st

BASE = os.path.join(os.path.dirname(__file__), "..", "runs_v2")
MODES = ["stream_only", "datagram_only", "hybrid"]
CONDS = ["clean", "rtt100", "rtt300", "rtt500", "loss2", "loss10"]


def rows(d, name):
    f = os.path.join(d, name)
    return list(csv.DictReader(open(f))) if os.path.exists(f) else []


def nums(rs, c, pos=True):
    out = []
    for r in rs:
        try:
            x = float(r.get(c, "") or "nan")
            if x == x and (not pos or x > 0):
                out.append(x)
        except ValueError:
            pass
    return out


def p95(v):
    if not v:
        return 0
    v = sorted(v)
    return v[min(len(v) - 1, int(0.95 * len(v)))]


def rx(d):
    f = os.path.join(d, "receiver_dgram_summary.json")
    if not os.path.exists(f):
        return None, None
    try:
        conns = json.load(open(f)).get("connections", {})
    except ValueError:
        return None, None
    loss, owdv = [], []
    for c in conns.values():
        for trk in c.values():
            if isinstance(trk, dict):
                if "loss_pct" in trk:
                    loss.append(trk["loss_pct"])
                if "owdv_ms_p95" in trk:
                    owdv.append(trk["owdv_ms_p95"])
    return (round(st.mean(loss), 1) if loss else None,
            round(max(owdv), 1) if owdv else None)


def summarize(d):
    tel = rows(d, "telemetry.csv")
    if not tel:
        return None
    tx = rows(d, "tx_events.csv")
    rtt = nums(tel, "rtt_ms")
    e2e = nums(tx, "e2e_us", pos=False)
    qw = nums(tx, "qwait_us", pos=False)
    rl, owdv = rx(d)
    return {
        "rtt_avg": round(st.mean(rtt)) if rtt else 0,
        "rtt_p95": round(p95(rtt)) if rtt else 0,
        "fresh_p95": round(p95(e2e) / 1000) if e2e else 0,
        "qw_p95": round(p95(qw) / 1000, 1) if qw else 0,
        "dlost": int(max(nums(tel, "datagrams_lost", pos=False) or [0])),
        "shed": int(max(nums(tel, "datagram_expired", pos=False) or [0]))
              + int(max(nums(tel, "datagram_evicted", pos=False) or [0])),
        "sfail": int(max(nums(tel, "stream_send_fail", pos=False) or [0])),
        "rx_loss": rl,
        "owdv_p95": owdv,
    }


METRICS = [
    ("rtt_avg", "RTT avg (ms)"),
    ("rtt_p95", "RTT p95 (ms)"),
    ("fresh_p95", "Freshness e2e p95 (ms)"),
    ("qw_p95", "Queue wait p95 (ms)"),
    ("dlost", "Datagrams lost — ACK (count)"),
    ("shed", "Shed: expired+evicted (count)"),
    ("sfail", "Stream send fail (count)"),
    ("rx_loss", "Rx gap-loss % (telemetry track)"),
    ("owdv_p95", "AoI proxy — OWDV p95 (ms)"),
]


def main():
    cache = {}
    for m in MODES:
        for c in CONDS:
            ds = sorted(glob.glob(os.path.join(BASE, f"sweep_{m}_{c}_r*")))
            vs = [summarize(d) for d in ds]
            cache[(m, c)] = [v for v in vs if v]
    for key, label in METRICS:
        print(f"\n### {label}")
        hdr = f"{'cond':<9}" + "".join(f"{m.replace('_only',''):>16}" for m in MODES)
        print(hdr + "\n" + "-" * len(hdr))
        for c in CONDS:
            cells = []
            for m in MODES:
                vals = [v[key] for v in cache[(m, c)] if v.get(key) is not None]
                if not vals:
                    cells.append("-")
                elif len(vals) == 1:
                    cells.append(f"{vals[0]:.1f}(n=1)")
                else:
                    # mean +/- sample std; n reps
                    cells.append(f"{st.mean(vals):.1f}±{st.stdev(vals):.1f}")
            print(f"{c:<9}" + "".join(f"{x:>16}" for x in cells))


if __name__ == "__main__":
    main()
