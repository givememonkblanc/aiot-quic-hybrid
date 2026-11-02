import json,sys
from pathlib import Path
from analyze_sweep import analyze
base=Path(__file__).resolve().parent
paths=[Path(x).resolve() for x in sys.argv[1:]]
rows=[r for p in paths for r in analyze(p)]
(base/'sweep_summary.json').write_text(json.dumps(rows,indent=2)+'\n')
def f(v):return '—' if v is None else f'{v:.2f}'
lines=['# Fixed JPEG core × priority results','',
'Each cell: 90 seconds, first 15 seconds excluded. Fixed 145989-byte JPEG, nominal 10 Hz replay and 1 Hz image attempt. Existing person FOMO; this is not TinyVLM. TX core1/priority5. No added network impairment.','',
'| AI core | Priority | N | AI mean / p95 / p99 (s) | Source-to-finish p95 (s) | Complete JPEG KiB/s | Replay FPS | Missed input slots | CPU0 / CPU1 % |',
'|---|---|---|---|---|---|---|---|---|']
for s in rows:
 lines.append(f'| {s["core"]} | {s["priority"]} | {s["n"]} | '+ ' / '.join(f(s[k]) for k in ['ai_mean_s','ai_p95_s','ai_p99_s'])+f' | {f(s["source_age_p95_s"])} | {s["goodput_KiBs"]:.1f} | {s["replay_fps"]:.2f} | {s["replay_missed"]} | '+' / '.join(f(x) for x in s['cpu_pct'])+' |')
lines+=['','## Interpretation','',
'At this offered workload, core1/priority1 is a promising static configuration: mean AI 1.06s, complete-image goodput 70.3 KiB/s, no nominal replay-slot misses. Core0/priority1 also preserves goodput while reducing inference time, but misses some input slots. This weakens the case for a new dynamic manager at this load; it does not prove robustness across loads or repeated runs.',
'Raising priority to 5/6 trades away service: core0 input supply falls to about 1.3 FPS and image goodput falls; core1 yields zero verified complete JPEGs despite faster inference. Completion latency alone would select misleading settings. The exact transport/scheduler wait mechanism remains uninstrumented.','']
lines+=['','## Delivery and memory','', '| Core / priority | Image attempts / rejected | Accepted / complete cohort | AI offers / overwritten | MQTT results received | Peak queue KiB | Peak PSRAM MiB | Min internal free KiB |','|---|---|---|---|---|---|---|---|']
for s in rows:
 lines.append(f'| {s["core"]}/{s["priority"]} | {s["image_attempts"]}/{s["image_admission_rejected"]} | {s["image_accepted"]}/{s["accepted_cohort_completed"]} | {s["ai_offers"]}/{s["pending_overwrites"]} | {s["mqtt_received"]} | {f(s["queue_peak"]/1024) if s["queue_peak"] is not None else "—"} | {f(s["psram_peak"]/2**20) if s["psram_peak"] is not None else "—"} | {f(s["min_internal_free"]/1024) if s["min_internal_free"] is not None else "—"} |')
lines+=['','Complete cohort: JPEG accepted in the observation window and received by window end +8 seconds; contiguous bytes and SHA256 must match. Outstanding/noncomplete frames are not labelled network packet loss. Admission rejects, incomplete accepted images, AI pending replacements and nominal input misses are separate.','',
'## Characterization grid','',
'Cells list static configurations with all observed completed jobs within the source-to-finish deadline AND measured complete-image goodput above the threshold. No application requirement was supplied. These are descriptive pass cells, not guarantees. They do not require all nominal inputs to be processed.','',
'| Deadline | ≥32 KiB/s | ≥64 KiB/s | ≥96 KiB/s |','|---|---|---|---|']
for d in (1,2,3,5):
 vals=[]
 for g in (32,64,96):
  passing=[f'C{s["core"]}/P{s["priority"]}' for s in rows if s['status']=='completed' and s['priority_verified'] and any(x['deadline_s']==d and x['min_goodput_KiBs']==g and x['meets'] for x in s['grid'])]
  vals.append(', '.join(passing) or 'none observed')
 lines.append(f'| {d}s | '+' | '.join(vals)+' |')
lines+=['','## Task runtime (% of one core)','', '| Core / priority | AI | main | camera | TX | Wi-Fi |','|---|---|---|---|---|---|']
for s in rows:
 lines.append(f'| {s["core"]}/{s["priority"]} | '+' | '.join(f(s['tasks'].get(n,{}).get('pct')) for n in ['camera_ai','main','cam_task','net_tx','wifi'])+' |')
lines+=['','## Limits','',
'- Descriptive tails from a single short pass; report N, do not treat p99 as a validated tail bound or observed maximum as WCET.',
'- Single fixed frame, no object/event ground truth: no F1, recall or end-to-end event deadline success can be claimed.',
'- Replay FPS measures supplied fixture frames, not physical capture FPS. Camera background processing remains enabled; app camera acquisition/copy is bypassed. Main starvation reduces actual supplied frames despite the same nominal schedule.',
'- Completed-job latency may improve while input slots or pending frames are dropped. Evaluate these jointly.',
'- Core runs require different firmware/reboots; reversed priority order mitigates but does not remove temporal/network/order confounds. No repeats here.',
'- Aggregate CPU cost per complete-image byte includes AI and common overhead; it is a system cost, not isolated QUIC cost.',
'- Success at one static workload does not cover the whole workload space. Failure here does not establish that dynamic control can overcome capacity limits.',
'- See SWEEP_PROTOCOL.md and machine-readable sweep_summary.json for detailed metrics, per-task priorities, conditional deadline fractions and firmware manifests.', '', '## Raw runs','']
lines.extend(f'- [{p.name}]({p.relative_to(base)}/outcome.json)' for p in paths)
(base/'SWEEP_RESULTS.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines[:8+len(rows)]))
