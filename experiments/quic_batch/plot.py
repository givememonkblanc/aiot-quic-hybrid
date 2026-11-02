import json,sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
p=Path(sys.argv[1]);ss=json.loads((p/'aggregate.json').read_text());x=np.arange(len(ss));labels=[str(s['batch_kib'])+' KiB' if s['batch_kib'] else 'Frame' for s in ss]
fig,axs=plt.subplots(2,2,figsize=(10,7),layout='constrained')
ax=axs[0,0];ax.bar(x-.18,[s['submission_calls_per_sent_frame'] for s in ss],.36,label='Batch API calls');ax.bar(x+.18,[s['locks_per_sent_frame'] for s in ss],.36,label='Image TX lock acquisitions');ax.axhline(121,color='gray',linestyle=':',label='DATAGRAMs/frame');ax.set_ylabel('Count per sent-frame equivalent');ax.legend(fontsize=8)
ax=axs[0,1];means=np.array([s['goodput_KiBs'] for s in ss]);low=np.array([s['goodput_repeat_range'][0] for s in ss]);high=np.array([s['goodput_repeat_range'][1] for s in ss]);ax.bar(x,means,color='#3484b8');ax.errorbar(x,means,yerr=[means-low,high-means],fmt='none',ecolor='black',capsize=4);ax.set_ylabel('Received complete JPEG KiB/s');ax.set_title('Whiskers: two-run range, not CI',fontsize=9)
ax=axs[1,0];ax.bar(x-.18,[s['ai_ms_mean']/1000 for s in ss],.36,label='Mean');ax.bar(x+.18,[s['ai_ms_p95']/1000 for s in ss],.36,label='p95');ax.set_ylabel('Successful AI latency (s)');ax.legend(fontsize=8)
ax=axs[1,1];ax.bar(x,[s['quic_cpu_ms_per_received_KiB'] for s in ss],color='#7e5ba5');ax.set_ylabel('QUIC I/O task CPU ms / received KiB')
for ax in axs.flat:ax.set_xticks(x,labels);ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
fig.suptitle('ESP32-S3: fixed 1024×1024 JPEG, ESPDet + QUIC DATAGRAM batching')
fig.savefig(p/'comparison.png',dpi=180);fig.savefig(p/'comparison.pdf');plt.close(fig)
