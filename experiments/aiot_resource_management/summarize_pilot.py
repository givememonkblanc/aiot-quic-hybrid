"""Summarize observed queue-policy pilot metrics, preserving unavailable KPIs."""
import csv
import json
import statistics
import sys
from pathlib import Path

p = Path(sys.argv[1])
summary = json.loads((p/'summary.json').read_text())
outcome = json.loads((p/'outcome.json').read_text())
raw = [json.loads(s) for s in (p/'samples.jsonl').read_text().splitlines()]
lines = ['# Device resource-management pilot', '',
         'Live ESP32 measurement; queue-policy ablation, not a completed cross-layer resource manager evaluation.', '',
         'Requested camera rate 30 FPS; payload queue budget 6 MiB; existing firmware/model unchanged. Sequence: FIFO / adaptive / adaptive / FIFO, 45 seconds each; first 10 seconds excluded per stage. Same configured limits, but scene/network trajectories are not identical replay. No network impairment injected.', '',
         '| Stage | Mode | Capture FPS | AI received / 35s (approx.) | AI receive FPS | Sampled PSRAM peak MiB | Sampled queue peak MiB | Internal min free KiB |',
         '|---|---|---:|---:|---:|---:|---:|---:|']
for s in summary:
    lines.append(f'| {s["stage"]} | {"FIFO" if s["mode"]==1 else "adaptive policy 1"} | {s["capture_fps"]:.2f} | {s["ai_received_count"]} | {s["ai_received_fps"]:.3f} | {s["psram_peak_sampled_used"]/2**20:.2f} | {s["queue_peak_sampled"]/2**20:.2f} | {s["internal_min_free"]/1024:.1f} |')
lines += ['', '## Interpretation limits', '',
          '- AI receive FPS is not inference FPS, event recall or deadline success. Ground-truth event labels, a complete sender event ledger and clock synchronization are unavailable. Existing gen_ts_ms and receiver timestamps alone do not establish event-to-receiver deadlines.',
          '- The ai_us telemetry wraps ai_inference_run (including its processing), not only model Invoke. Latest values are sampled and can repeat; no inference p95/p99 is inferred from them.',
          '- RAM samples arrive about every 5 seconds. Peaks are observed sample maxima, not exact allocation high-water marks. Boot-lifetime minimum heap counters include earlier experiments and are excluded from trial minima.',
          '- F1/mAP, energy, packet loss, complete-frame goodput, handover recovery and causal performance improvement are not established by this pilot.',
          '- FIFO/ adaptive switching can carry queue and connection state across phases. ABBA ordering mitigates simple ordering bias but does not create independent repeated trials.', '',
          f'Status: {outcome["status"]}. Original settings restored and telemetry-confirmed: {outcome.get("restore_confirmed", False)}.', '', '## Additional observations', '']
for s in summary:
    vals=s['latest_inference_ms_samples']
    lines.append(f'- Stage {s["stage"]}: sampled latest AI processing duration {min(vals)/1000:.2f}–{max(vals)/1000:.2f} s; {s["unique_ram_samples"]} distinct RAM samples; RTT samples {min(s["rtt_ms_samples"])}–{max(s["rtt_ms_samples"])} ms.')
cids = sorted({s['ram']['cid'] for s in raw})
lines += ['', f'{len(cids)} distinct connection IDs observed in RAM telemetry. This is not an AP-handover count.', '',
          'Next: identical event-labeled sensor replay, per-event source/admission/drop/receive logging with clock mapping, then fixed-AI/network-only/joint-controller comparison under a repeatable network trace.']
(p/'REPORT.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
