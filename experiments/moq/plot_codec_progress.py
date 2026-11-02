#!/usr/bin/env python3
"""Run-level comparison; v23 is a post-screening follow-up, not randomised with v22."""
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parents[2]
base=ROOT/'experiments/moq/results'
a=json.loads((base/'codec-v22-screen/comparison.json').read_text())
b=json.loads((base/'codec-v23-overlap/comparison.json').read_text())
assert not a['invalid'] and not b['invalid']
assert a['valid_runs']==4 and b['valid_runs']==2
rows=sorted(a['rows'],key=lambda r:({'sync_low':0,'async_low':1,'sync_medium':2,'async_medium':3}[r['policy']]))+b['rows']
assert len({tuple(r['ai_detection_signatures']) for r in rows})==1
assert len({tuple(r['image_signatures']) for r in rows if 'medium' in r['policy']})==1
labels=[('overlap\nmedium\nr'+str(r['repeat'])) if r['pipeline'] else r['policy'].replace('_','\n') for r in rows]
fig,axes=plt.subplots(1,2,figsize=(11,4))
x=list(range(len(rows)));w=.25
for shift,key,label,color in [(-w,'ai','AI received','#adb5bd'),(0,'evidence','Evidence paired','#60a5fa'),(w,'pairs_confirmed_on_time_lower','Pairs ACK≤10 s','#166534')]:
 axes[0].bar([v+shift for v in x],[r[key] for r in rows],w,label=label,color=color)
axes[0].set_xticks(x,labels);axes[0].set_ylim(0,28);axes[0].set_ylabel('Result objects / matching images');axes[0].legend(fontsize=8,loc='upper left',ncol=3)
axes[1].plot(x,[r['ai_ack_p95_s'] for r in rows],'o',color='#1d4ed8');axes[1].set_xticks(x,labels);axes[1].set_ylabel('AI source→ACK p95 (s)');axes[1].set_ylim(0,8)
for ax in axes:
 ax.axvline(3.5,color='gray',linestyle='--',linewidth=1);ax.grid(axis='y',alpha=.2)
fig.suptitle('128 kbit/s, same AI pixels and same medium JPEG bytes; individual runs',fontsize=11)
fig.text(.5,.01,'v22 screening (left four settings) → selected v23 follow-up (right two); no physical screw GT',ha='center',fontsize=9)
fig.tight_layout(rect=(0,.05,1,.94))
out=ROOT/'papers/mission_aiot_moq';fig.savefig(out/'codec-progress.pdf');fig.savefig(out/'codec-progress.png',dpi=160)
(out/'codec-progress.json').write_text(json.dumps(dict(scope='descriptive, post-screening follow-up',rows=rows),indent=2)+'\n')
print(out/'codec-progress.pdf')
