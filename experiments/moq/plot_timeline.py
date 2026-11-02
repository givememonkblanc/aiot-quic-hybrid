#!/usr/bin/env python3
"""Receiver-time media timeline with actual host-applied bandwidth steps."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser();p.add_argument('suite',type=Path);a=p.parse_args()
m=json.loads((a.suite/'manifest.json').read_text())
policies=['high','high_timeout','low','medium','adaptive']
cells=[c for c in m['results'] if c['status']=='complete' and c['network']=='step']
cells.sort(key=lambda c:policies.index(c['policy']))
if not cells:raise SystemExit('No completed step trials')
fig,axes=plt.subplots(len(cells),1,figsize=(11,2.1*len(cells)),sharex=True,squeeze=False)
for ax,cell in zip(axes[:,0],cells):
    d=a.suite.parent/cell['run'];t=json.loads((d/'manifest.json').read_text())
    e=[json.loads(x) for x in (d/'events.jsonl').read_text().splitlines() if x.strip()]
    start=max(x['t'] for x in e if x['event']=='subscribed')
    images=[x for x in e if x['event']=='object' and x.get('alias')==1]
    resets=[x for x in e if x['event']=='reset' and x.get('alias')==1]
    ax.scatter([x['t']-start for x in images],[x['width'] for x in images],s=12,color='#2266ad',label='complete JPEG')
    ax.scatter([x['t']-start for x in resets],[0]*len(resets),s=22,marker='x',color='#c33',label='reset')
    changes=t.get('rate_changes',[])
    if len(changes)==2:ax.axvspan(changes[0]['actual_s'],changes[1]['actual_s'],color='#e8b647',alpha=.2,label='128 kbit/s')
    ax.set_title(cell['policy'],loc='left',fontsize=11);ax.set_yticks([0,400,800,1600]);ax.set_ylabel('Width (px)');ax.grid(alpha=.2)
axes[0,0].legend(loc='upper right',fontsize=8)
axes[-1,0].set_xlabel('Seconds since receiver subscription completion (not source-clock age)')
fig.suptitle('512 → 128 → 512 kbit/s: received representations and resets\nOne run per policy; shaded boundaries use actual shaper update timestamps')
fig.tight_layout(rect=[0,0,1,.95]);fig.savefig(a.suite/'timeline.pdf');fig.savefig(a.suite/'timeline.png',dpi=180)
print(a.suite/'timeline.pdf')
