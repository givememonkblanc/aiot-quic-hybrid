"""Aggregate per-run diagnostics without pooling latency samples."""
import json,re
from pathlib import Path
from analyze_policies import summary
BASE=Path(__file__).resolve().parent
OUT=BASE/'results/repeat-v6'
manifest=json.loads((OUT/'manifest.json').read_text())
rows=[];analysis_exclusions=[]
for policy,old in [('A0','A0-retry'),('S0','S0'),('S1','S1'),('S2','S2'),('S3','S3'),('S4','S4')]:
 rows.append(dict(policy=policy,network='unshaped',replicate=1,run='policy-v6-'+old))
for r in manifest['results']:
 if r['status']=='valid' or (r.get('outcome_status')=='passed' and r.get('reason','').startswith('Analysis accounting invalid:')):
  try:summary(BASE/'results'/r['run'])
  except AssertionError:continue
  rows.append(dict(policy=r['policy'],network=r['network'],replicate=2 if r['network']=='unshaped' else 1,run=r['run'],analysis_status='original_valid' if r['status']=='valid' else 'source_ledger_reconciled'))
validated=[]
for row in rows:
 run=BASE/'results'/row['run']
 try:d=summary(run)
 except AssertionError as e:
  analysis_exclusions.append(dict(run=row['run'],reason=str(e)));continue
 validated.append(row)
 assert d['outcome']['status']=='passed' and d['outcome']['restored']
 row.update(summary=d)
 if row['network']=='limited':
  text=(OUT/(row['run']+'-shaper-end.txt')).read_text()
  match=re.search(r'Sent (\d+) bytes (\d+) pkt \(dropped (\d+)',text)
  assert match and int(match[1])>0,'No evidence of traffic through impairment'
  row['shaper']=dict(bytes=int(match[1]),packets=int(match[2]),drops=int(match[3]))
rows=validated
(OUT/'comparison.json').write_text(json.dumps(dict(rows=rows,analysis_exclusions=analysis_exclusions,attempts=manifest['results']),indent=2)+'\n')
lines=['| Network | Policy | Run | JPEG received/offered | JPEG age p95 s | AI age p95 s | AI admission→ACK p95 s | AI ACK≤5s/produced | Preview goodput kbit/s¹ |','|---|---|---:|---:|---:|---:|---:|---:|---:|']
for row in sorted(rows,key=lambda r:(r['network'],r['policy'],r['replicate'])):
 d=row['summary'];v=d['preview'];a=d['ai'];lines.append(f"| {row['network']} | {row['policy']} | {row['replicate']} | {v['received']}/{v['source_count']} | {v['ack_age_us']['p95']/1e6:.3f} | {a['ack_age_us']['p95']/1e6:.3f} | {a['admission_to_ack_us']['p95']/1e6:.3f} | {a['deadline_acknowledged']}/{a['source_count']} | {v['receiver_window_goodput_bps']/1000:.1f} |")
lines+=['','¹ Complete JPEG payload bytes, receiver first subscription→finished window; includes drain, not steady-state link capacity.','ACK age is a same-device-clock proxy, not exact receiver latency.','']
(OUT/'table.md').write_text('\n'.join(lines))
print('\n'.join(lines))
