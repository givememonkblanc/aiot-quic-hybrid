"""Reconcile every source preview and every actual AI result against one terminal state."""
import collections,json,re,zlib,struct
from pathlib import Path

def validate(run):
 rows=json.loads((run/'device-events.json').read_text());meta=json.loads((run/'audit-manifest.json').read_text());raw=(run/'audit.bin').read_bytes()
 assert len(raw)==meta['bytes'] and zlib.crc32(raw)==meta['crc']
 assert not(meta['overflow'] or meta['truncated'] or meta['late'])
 decoded=[];pos=0
 while pos<len(raw):
  assert pos+2<=len(raw);n=struct.unpack_from('<H',raw,pos)[0];pos+=2
  assert 0<n<512 and pos+n<=len(raw);decoded.append(raw[pos:pos+n].decode().strip());pos+=n
 assert len(decoded)==meta['records'] and decoded==[x['line'] for x in rows],'JSON records differ from original journal'
 def tag(t):return [json.loads(x['line'].split(t+' ',1)[1]) for x in rows if t+' {' in x['line']]
 def unique(a,key):
  ids=[x[key] for x in a];assert len(ids)==len(set(ids)),'duplicate '+key;return set(ids)
 captures=tag('[MOQ_CAPTURE]');source=unique(captures,'seq');replay=unique(tag('[REPLAY]'),'seq')
 final=[int(m[1]) for r in rows if (m:=re.search(r'\[MOQ_CAPTURE_END\] count=(\d+)',r['line']))]
 assert len(final)==1 and len(source)==final[0] and source==replay,'source ledger incomplete'
 profiles=tag('[AI_PROFILE]');outcomes=tag('[AI_OUTCOME]');starts=tag('[AI_START]');produced=tag('[MOQ_AI_PRODUCED]');phases=tag('[ESPDET_PHASE]')
 assert unique(profiles,'seq')==unique(outcomes,'seq')==unique(starts,'seq'),'inference lifecycle mismatch'
 success={x['seq'] for x in outcomes if x['success']};assert success==unique(produced,'seq')==unique(phases,'seq'),'inference result mismatch'
 assert success<=source
 terminal={1:collections.defaultdict(list),2:collections.defaultdict(list)}
 for x in tag('[MOQ_ADMISSION]'):
  if x['action']=='replace':terminal[1][x['old_seq']].append('replaced')
  elif x['action'] in ('reject_new','cache_miss'):terminal[1][x['seq']].append(x['action'])
 expires={tuple(map(int,m.groups())) for r in rows if (m:=re.search(r'\[MOQ_EXPIRED\] alias=(\d+) group=(\d+)',r['line']))}
 for alias,g in expires:terminal[alias][g].append('expired')
 for x in tag('[MOQ_OBJECT]'):
  if not x['accepted'] and (1,x['group']) not in expires:terminal[1][x['group']].append('publish_rejected')
 rx=[json.loads(x) for x in (run/'events.jsonl').read_text().splitlines()];rx=[x for x in rx if x['event']=='object']
 for x in rx:terminal[x['alias']][x['group']].append('received')
 for x in tag('[MOQ_PENDING]'):terminal[x['alias']][x['group']].append('pending_at_cutoff')
 for x in produced:
  if not x['enqueued']:terminal[2][x['seq']].append('queue_rejected')
 assert set(terminal[1])==source,'preview terminal ID mismatch'
 assert set(terminal[2])==success,'AI terminal ID mismatch'
 for t in terminal.values():assert all(len(states)==1 for states in t.values()),'duplicate terminal state'
 return dict(source_count=len(source),ai_attempts=len(outcomes),ai_success=len(success),ai_failures=sum(not x['success'] for x in outcomes),terminal_counts={str(k):dict(collections.Counter(s[0] for s in t.values())) for k,t in terminal.items()},journal=meta)
if __name__=='__main__':
 import sys
 print(json.dumps(validate(Path(sys.argv[1])),indent=2))
