#!/usr/bin/env python3
"""Median JPEG size of the camera frames of a run (MOQ_CAPTURE lines of serial.log), in bytes.
Under the room light the frames of the turntable take 65 to 110 kB; in the dark the sensor gain
fills them with noise and they take 190 to 260 kB, and frames past 256 kB are dropped as invalid.
usage: frame_light.py serial.log   -> prints the median, or 0 without frames"""
import json
import re
import statistics
import sys

sizes = []
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"\[MOQ_CAPTURE\] (\{.*\})", line)
    if m:
        try:
            sizes.append(json.loads(m.group(1))["bytes"])
        except (ValueError, KeyError):
            pass
print(int(statistics.median(sizes)) if sizes else 0)
