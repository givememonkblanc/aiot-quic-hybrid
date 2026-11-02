#!/usr/bin/env python3
"""The largest number of passages in a serial record with less than GAP s (default 15) between two of
them.
usage: longest_burst.py serial.log [GAP]"""
import sys

times = [float(line.split()[0]) for line in open(sys.argv[1], errors="replace") if "[MOQ_PASSAGE]" in line]
gap = float(sys.argv[2]) if len(sys.argv) > 2 else 15
best, n = 0, 0
for i, t in enumerate(times):
    n = n + 1 if i and t - times[i - 1] <= gap else 1
    best = max(best, n)
print(best)
