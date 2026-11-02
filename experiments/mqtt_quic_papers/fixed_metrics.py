"""Metrics for identical scheduled-input replay, not packet-level handoff metrics."""
from collections import Counter
from paper_metrics import quantile


def summarize(raw, config):
    generated = raw['generated']
    if len(generated) != config['count'] or [g['seq'] for g in generated] != list(range(config['count'])):
        raise ValueError('incomplete or corrupt scheduled input')
    for g in generated:
        if g['due_us'] != g['seq']*1_000_000//config['rate']:
            raise ValueError('different generation schedule')
    rx = sorted(raw['received'], key=lambda r: r['received_us'])
    unique = {}
    for r in rx:
        if not 0 <= r['seq'] < config['count'] or r['qos'] != config['qos']:
            raise ValueError('invalid receiver sequence or QoS')
        expected = r['received_us']-generated[r['seq']]['due_us']
        if expected < 0 or r['latency_us'] != expected:
            raise ValueError('invalid receive clock')
        unique.setdefault(r['seq'], r)
    latencies = [r['latency_us']/1000 for r in unique.values()]
    lateness = [(g['generated_us']-g['due_us'])/1000 for g in generated]
    deadline_count = sum(v <= config['deadline_ms'] for v in latencies)
    submitted = raw['submitted']
    submitted_ids = [s['seq'] for s in submitted]
    if len(set(submitted_ids)) != len(submitted_ids):
        raise ValueError('duplicate publish submission')
    resume_us = (config['pause_at_ms']+config['pause_ms'])*1000
    after = [r for r in unique.values() if r['received_us'] >= resume_us]
    before = [r for r in unique.values() if r['received_us'] < config['pause_at_ms']*1000]
    pause_gap = (min(r['received_us'] for r in after)-max(r['received_us'] for r in before))/1000 if before and after and config['pause_ms'] else None
    fresh_after = [r for r in after if generated[r['seq']]['due_us'] >= resume_us]
    return dict(generated=len(generated), submitted_ok=sum(s['ok'] for s in submitted),
                publish_errors=sum(not s['ok'] for s in submitted), received_unique=len(unique),
                duplicates=len(rx)-len(unique), invalid_payloads=raw['invalid_payloads'],
                delivered_fraction=len(unique)/len(generated), deadline_received=deadline_count,
                deadline_fraction=deadline_count/len(generated),
                received_p95_ms=quantile(latencies,95) if latencies else None,
                received_p99_ms=quantile(latencies,99) if latencies else None,
                generation_lateness_p99_ms=quantile(lateness,99), generation_lateness_max_ms=max(lateness),
                decisions=dict(Counter(g['decision'] for g in generated)),
                queue_peak_bytes=raw['application_queue_peak_bytes'],
                application_pending_bytes_at_cutoff=raw['application_pending_bytes_at_cutoff'],
                receive_gap_across_application_pause_ms=pause_gap,
                resume_to_first_receive_ms=(min(r['received_us'] for r in after)-resume_us)/1000 if after and config['pause_ms'] else None,
                resume_to_first_new_generation_receive_ms=(min(r['received_us'] for r in fresh_after)-resume_us)/1000 if fresh_after and config['pause_ms'] else None,
                worker_pending_at_cutoff=raw['worker_pending_at_cutoff'])
