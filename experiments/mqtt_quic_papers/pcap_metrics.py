"""Compute wire KPIs from a supplied capture, never application byte counters.

Capture one interface/observation point. For TtFDF, give frame numbers identified
from a decrypted trace: publisher Initial and the packet containing first PUBLISH.
The operator must verify these markers; this script cannot infer encrypted MQTT.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def summarize_packets(rows, initial=None, publish=None):
    if not rows:
        raise ValueError('No packets matched; check flow filter and capture window')
    if (initial is None) != (publish is None):
        raise ValueError('Both Initial and first PUBLISH frame numbers are required')
    times = {int(number): float(timestamp) for number, timestamp, _ in rows}
    if initial is not None:
        if initial not in times or publish not in times:
            raise ValueError('Marker frame not in selected capture flow/window')
        if times[publish] < times[initial]:
            raise ValueError('PUBLISH precedes Initial')
    return dict(wire_packets=len(rows), wire_bytes=sum(int(length) for _, _, length in rows),
                capture_span_s=max(times.values())-min(times.values()),
                packet_initial_to_first_publish_ms=(times[publish]-times[initial])*1000
                    if initial is not None else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcap', type=Path)
    parser.add_argument('--filter', required=True, help='tshark display filter isolating both directions of ONE endpoint pair/window')
    parser.add_argument('--initial-frame', type=int)
    parser.add_argument('--publish-frame', type=int)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = subprocess.run(['tshark', '-n', '-r', str(args.pcap), '-Y', args.filter,
                             '-T', 'fields', '-e', 'frame.number', '-e', 'frame.time_epoch',
                             '-e', 'frame.len'], check=True, capture_output=True, text=True)
    rows = [line.split('\t') for line in result.stdout.splitlines() if line]
    metrics = summarize_packets(rows, args.initial_frame, args.publish_frame)
    metrics.update(capture_sha256=hashlib.sha256(args.pcap.read_bytes()).hexdigest(),
                   display_filter=args.filter, byte_definition='sum(frame.len), captured link-layer frame lengths',
                   initial_frame=args.initial_frame, publish_frame=args.publish_frame,
                   marker_method='operator-verified decrypted packet frame numbers; not inferred',
                   status='measured_from_supplied_capture')
    with args.output.open('x') as stream:
        json.dump(metrics, stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
