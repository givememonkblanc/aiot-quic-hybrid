"""Integrate external-meter CSV (timestamp_s,voltage_v,current_a).

Input must cover exactly the desired measurement window. Reports whole measured
device energy, not process-attributed PowerTop estimates or inferred CPU power.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def integrate(samples):
    if len(samples) < 2:
        raise ValueError('At least two external meter samples required')
    if any(not math.isfinite(x) for sample in samples for x in sample):
        raise ValueError('Non-finite meter reading')
    if any(v < 0 or a < 0 for _, v, a in samples):
        raise ValueError('Expected nonnegative consumption readings')
    energy = 0
    for (t0, v0, a0), (t1, v1, a1) in zip(samples, samples[1:]):
        if t1 <= t0:
            raise ValueError('Meter timestamps must strictly increase')
        energy += (v0*a0 + v1*a1)*0.5*(t1-t0)
    duration = samples[-1][0]-samples[0][0]
    return dict(energy_j=energy, power_mean_w=energy/duration, measurement_s=duration,
                samples=len(samples), method='trapezoidal integral of external V*I samples',
                scope='device on external meter; no idle subtraction; not process PowerTop')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    with args.csv.open() as stream:
        samples = [tuple(float(r[k]) for k in ('timestamp_s', 'voltage_v', 'current_a'))
                   for r in csv.DictReader(stream)]
    result = integrate(samples)
    result['source_sha256'] = hashlib.sha256(args.csv.read_bytes()).hexdigest()
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
