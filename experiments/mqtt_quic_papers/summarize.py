"""Aggregate independent run summaries; never pool rolling-window percentiles."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
args = parser.parse_args()
data = json.loads((args.directory/'summary.json').read_text())
groups = defaultdict(list)
for row in data:
    groups[(row['transport'], row['qos'], row.get('measurement_pass', 'legacy'),
            row['sent'], row.get('payload_bytes'), row.get('interval_ms'))].append(row)
output = []
for (transport, qos, measurement_pass, count, payload, interval), rows in sorted(groups.items()):
    latencies = [r['latency_p95_ms'] for r in rows]
    metrics = {}
    for field in ('latency_mean_ms', 'latency_p5_ms', 'latency_p25_ms', 'latency_p50_ms',
                  'latency_p75_ms', 'latency_p95_ms', 'latency_p99_ms',
                  'latency_rsd_population', 'latency_rsd_sample', 'publisher_execution_s',
                  'cpu_process_s', 'python_allocation_total_bytes', 'native_allocation_total_bytes'):
        values = [r[field] for r in rows if r.get(field) is not None]
        metrics[field] = dict(n=len(values), mean=statistics.mean(values) if values else None,
                             sample_sd=statistics.stdev(values) if len(values)>1 else None)
    output.append(dict(transport=transport, qos=qos, n=len(rows),
        measurement_pass=measurement_pass, count=count, payload_bytes=payload, interval_ms=interval,
        metrics=metrics,
        sent=sum(r['sent'] for r in rows), received_unique=sum(r['received_unique'] for r in rows),
        mean_of_run_p95_ms=statistics.mean(latencies),
        sample_sd_of_run_p95_ms=statistics.stdev(latencies) if len(rows)>1 else None,
        mean_connect_to_connack_ms=statistics.mean(r['connect_to_connack_ms'] for r in rows)))
(args.directory/'aggregate.json').write_text(json.dumps(output,indent=2)+'\n')
print(json.dumps(output,indent=2))
