"""Audit policy pilot cohorts. ACK age is an upper-bound proxy, not server AoI."""
import argparse,json,re
from pathlib import Path

def summary(run):
 rows=json.loads((run/'device-events.json').read_text());out=json.loads((run/'outcome.json').read_text())
 def tagged(tag):return [json.loads(r['line'].split(tag+' ',1)[1]) for r in rows if tag+' {' in r['line']]
 def stats(a):
  a=sorted(a)
  if not a:return None
  return dict(n=len(a),p50=a[int((len(a)-1)*.5)],p95=a[int((len(a)-1)*.95)],max=a[-1])
 receipts=tagged('[MOQ_RECEIPT]');admits=tagged('[MOQ_ADMITTED]');produced=tagged('[MOQ_AI_PRODUCED]');offers=tagged('[REPLAY]');caps=tagged('[MOQ_CAPTURE]');choices=tagged('[MOQ_POLICY_STATS]');cache=tagged('[MOQ_ADMISSION]')
 events=[json.loads(l) for l in (run/'events.jsonl').read_text().splitlines()];rx=[e for e in events if e['event']=='object']
 resets=[e for e in events if e['event']=='reset' and e.get('alias') in (1,2)]
 assert not any(e['event']=='reset' and e.get('alias') is None for e in events),'unattributed reset'
 expires=[tuple(map(int,m.groups())) for r in rows if (m:=re.search(r'\[MOQ_EXPIRED\] alias=(\d+) group=(\d+)',r['line']))]
 assert len({(x['alias'],x['group']) for x in rx})==len(rx),'duplicate object'
 terminal={(x['alias'],x['group']) for x in rx}|{(x['alias'],x['group']) for x in resets}
 assert terminal=={(x['alias'],x['group']) for x in admits},'admitted/terminal mismatch'
 assert {(x['alias'],x['group']) for x in rx}=={(x['alias'],x['group']) for x in receipts},'ACK receipt mismatch'
 assert len({p['seq'] for p in produced})==len(produced)==out['ai_completed'],'AI completion accounting mismatch'
 for n in (1,2):
  assert len([x for x in admits if x['alias']==n])==len({x['group'] for x in admits if x['alias']==n})
 capture_ids={x['seq'] for x in caps}
 assert len(capture_ids)==len(caps)==out['replay_offers'],'replay offer accounting mismatch'
 replay_ids={x['seq'] for x in offers}
 assert len(replay_ids)==len(offers),'duplicate replay record'
 source_ids=capture_ids|replay_ids
 totals=[int(m[1]) for r in rows if (m:=re.search(r'\[MOQ_CAPTURE_END\] count=(\d+)',r['line']))]
 assert len(totals)==1 and len(source_ids)==totals[0],'incomplete source ledger: unique IDs must match device final count'
 result=dict(source_ledger_count=len(source_ids),capture_log_missing=len(source_ids-capture_ids),capture_gap_trace_complete=source_ids==capture_ids,replay_log_missing=len(source_ids-replay_ids),run=run.name,outcome=out,actual_AI_results=len(produced),AI_event_queue_drops=sum(not p['enqueued'] for p in produced),replay_observed=len(offers),replay_missed_slots=sum(o['missed'] for o in offers),preview_replaced=sum(c['action']=='replace' for c in cache),preview_rejected_new=sum(c['action']=='reject_new' for c in cache),last_policy_stats=choices[-1] if choices else None)
 for alias,name in [(1,'preview'),(2,'ai')]:
  a=[x for x in admits if x['alias']==alias];r=[x for x in receipts if x['alias']==alias];b=[x for x in rx if x['alias']==alias];z=[x for x in resets if x['alias']==alias];denom=len(source_ids) if alias==1 else len(produced)
  result[name]=dict(admitted=len(a),received=len(b),received_bytes=sum(x['bytes'] for x in b),timeout_resets=len(z),timeout_reset_groups=sorted(x['group'] for x in z),unsent_expired=sum(x[0]==alias for x in expires),ack_age_us=stats([x['ack_us']-x['source_us'] for x in r]),admission_age_us=stats([x['admitted_us']-x['source_us'] for x in a]),source_count=denom,deadline_acknowledged=sum(x['ack_us']<=x['deadline_us'] for x in r),curve=[dict(deadline_ms=d,acknowledged=sum(x['ack_us']-x['source_us']<=d*1000 for x in r),source_count=denom) for d in (1000,2000,3000,5000)])
  admitted_at={x['group']:x['admitted_us'] for x in a}
  result[name]['admission_to_ack_us']=stats([x['ack_us']-admitted_at[x['group']] for x in r])
  if alias==2:
   delivered={x['group'] for x in b};reset={x['group'] for x in z};expired={g for al,g in expires if al==2};dropped={x['seq'] for x in produced if not x['enqueued']}
   result[name]['unaccounted_at_cutoff']=sorted({x['seq'] for x in produced}-delivered-reset-expired-dropped)
 start=min(e['t'] for e in events if e['event']=='subscribed')
 end=max(e['t'] for e in events if e['event']=='finished')
 result['receiver_window_s']=end-start
 result['preview']['receiver_window_goodput_bps']=result['preview']['received_bytes']*8/(end-start)
 result['interpretation']='Device source-to-ACK proxy, with omissions in denominators. No event GT, host clock synchronization, server AoI or mission DESR.'
 return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('run',type=Path);a=p.parse_args();print(json.dumps(summary(a.run),indent=2))
