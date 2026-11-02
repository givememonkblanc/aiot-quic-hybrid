#!/usr/bin/env python3
"""Short outages of a factory Wi-Fi link (roaming, interference): --outage arguments of
remote_ota_trial.py. The gaps between outages are exponential with the given mean and the
durations uniform between the bounds, from a seed, so that every method of a round meets the
same outages. usage: outages.py SEED [--span 120] [--gap 20] [--min 0.2] [--max 2]"""
import argparse
import random

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("seed", type=int)
p.add_argument("--span", type=float, default=120.0, help="seconds after the subscription")
p.add_argument("--gap", type=float, default=20.0, help="mean time between outages, s")
p.add_argument("--min", type=float, default=0.2)
p.add_argument("--max", type=float, default=2.0)
a = p.parse_args()
rng = random.Random(a.seed)
t, out = 5.0, []
while True:
    t += rng.expovariate(1 / a.gap)
    d = round(rng.uniform(a.min, a.max), 2)
    if t + d > a.span:
        break
    out.append(f"--outage={t:.2f}:{d}")
    t += d
print(" ".join(out))
