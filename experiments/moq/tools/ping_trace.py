#!/usr/bin/env python3
"""A one-way delay and loss trace of a real Wi-Fi link from `ping -D -O -i 0.05` to the device.

Each probe is placed at its send time. In each step of --step seconds the delay is half the
median round trip of the probes answered, and the loss the share of probes not answered;
a step with no answer keeps the last delay and loses everything. The round trip includes the
Wi-Fi hop both ways and the power saving of the station: half of it stands for the device to
host direction, which the shaper delays. usage: ping_trace.py PING.txt --out trace.csv"""
import argparse
import re
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("ping")
p.add_argument("--out", required=True)
p.add_argument("--step", type=float, default=0.1)
a = p.parse_args()
probes = {}   # seq -> (send time, rtt or None)
for line in open(a.ping):
    m = re.match(r"\[(\d+\.\d+)\].*icmp_seq=(\d+).*time=([\d.]+) ms", line)
    if m:
        t, seq, rtt = float(m[1]), int(m[2]), float(m[3])
        probes[seq] = (t - rtt / 1000, rtt)
        continue
    m = re.match(r"\[(\d+\.\d+)\] no answer yet for icmp_seq=(\d+)", line)
    if m:
        probes.setdefault(int(m[2]), (float(m[1]) - 1.0, None))   # -W 1: reported 1 s after sending
t0 = min(t for t, _ in probes.values())
steps = {}
for t, rtt in probes.values():
    steps.setdefault(int((t - t0) / a.step), []).append(rtt)
last, rows = 1.0, []
for k in range(max(steps) + 1):
    got = steps.get(k, [])
    answered = [r for r in got if r is not None]
    if answered:
        last = statistics.median(answered) / 2
    loss = 100.0 * (len(got) - len(answered)) / len(got) if got else 0.0
    if got and not answered:
        loss = 100.0
    rows.append(f"{k * a.step:.2f},{last:.2f},{loss:.1f}")
open(a.out, "w").write("t_s,delay_ms,loss_percent\n" + "\n".join(rows) + "\n")
lost = sum(1 for _, r in probes.values() if r is None)
rtts = sorted(r for _, r in probes.values() if r is not None)
print(f"{len(probes)} probes over {(max(steps) + 1) * a.step:.0f} s, {lost} unanswered ({100 * lost / len(probes):.2f}%), "
      f"round trip p50 {rtts[len(rtts) // 2]:.1f} p90 {rtts[int(.9 * len(rtts))]:.1f} p99 {rtts[int(.99 * len(rtts))]:.1f} "
      f"max {rtts[-1]:.1f} ms")
