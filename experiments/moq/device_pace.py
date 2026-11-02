#!/usr/bin/env python3
"""Mean duration of the inferences of a device run, from its serial record.

The device occasionally runs in a slow state in which inference and conversion take longer
for a whole run, whatever the method. Runs above the limit are repeated; the limit is
applied to every method alike. Exit status 1: above the limit."""
import re
import statistics
import sys
from pathlib import Path

LIMIT_S = 2.82


def pace(run):
    durations = [int(m.group(1)) / 1e6 for line in (Path(run) / "serial.log").read_text(errors="replace").splitlines()
                 if "[AI_PROFILE]" in line for m in [re.search(r'"duration_us":(\d+)', line)] if m]
    return statistics.mean(durations) if durations else None


if __name__ == "__main__":
    value = pace(sys.argv[1])
    print(f"{sys.argv[1]} {value}")
    sys.exit(1 if value is None or value > LIMIT_S else 0)
