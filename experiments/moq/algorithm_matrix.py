"""Generate a blocked, reproducible experiment plan; never flash or shape traffic."""
import argparse,hashlib,json,random
from pathlib import Path
POLICIES=[('A0','bounded_fifo','ai_first'),('S0','latest','fifo'),('S1','latest','ai_first'),('S2','latest','drr_bytes'),('S3','latest','edf'),('S4','latest','importance_edf'),('S5','latest','moqt21_priority')]
REFERENCES={
 'A0':'project retention baseline',
 'S0':'project no-priority baseline',
 'S1':'project strict-priority baseline',
 'S2':'RFC 8290 DRR-inspired track adaptation',
 'S3':'Liu-Layland EDF-inspired selector',
 'S4':'project single-path semantic/deadline proxy',
 'S5':'draft-ietf-moq-transport-21 section 5.1.2 adaptation',
}
NETWORKS=['unshaped','controlled_degradation','outage_recovery']
def make_plan(seed=20260922,repetitions=3):
 if repetitions<1:raise ValueError('repetitions must be positive')
 rng=random.Random(seed);runs=[]
 for repeat in range(1,repetitions+1):
  for network in NETWORKS:
   order=list(POLICIES);rng.shuffle(order)
   for policy,admission,selection in order:
    runs.append(dict(order=len(runs)+1,id=f'{network}-r{repeat}-{policy}',repeat=repeat,network=network,policy=policy,preview_admission=admission,selection=selection,reference=REFERENCES[policy],image_slots=1,ai_slots=1,status='planned_not_executed'))
 return dict(schema=3,seed=seed,repetitions=repetitions,execution_enabled=False,literature_map='LITERATURE_BASELINES.md',input_manifest_sha256=None,network_manifest_sha256=None,common_memory_budget_bytes=None,class_deadlines_ms=None,ablations=[dict(id='T0',mechanism='post_admission_delivery_timeout_reset',paired_base_policy='S1',timeout_reset=[0,1],status='implementation_ready_device_not_executed')],required_gates=['frozen_input_and_network_traces','common_memory_budget_and_deadlines','selectors_and_event_queue','timestamp_and_receiver_accounting','reconnect_for_outage'],runs=runs)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--seed',type=int,default=20260922);p.add_argument('--repetitions',type=int,default=3);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 plan=make_plan(a.seed,a.repetitions);raw=json.dumps(plan,indent=2)+'\n';a.out.parent.mkdir(parents=True,exist_ok=True)
 with a.out.open('x') as f:f.write(raw)
 print(json.dumps(dict(cells=len(plan['runs']),sha256=hashlib.sha256(raw.encode()).hexdigest(),status='planned_not_executed')))
