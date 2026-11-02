"""Trim ESP-DL registrations to the exact bundled model's operator table.
Only touches the isolated vendor checkout; rerunnable from its saved pristine header.
"""
import collections,hashlib,json,re,struct
from pathlib import Path
from flatbuffers.table import Table
BASE=Path(__file__).resolve().parent
model=BASE/'vendor/esp-dl/models/cat_detect/models/s3/espdet_pico_224_224_cat.espdl'
b=model.read_bytes()
assert b[:4]==b'EDL2' and struct.unpack_from('<I',b,4)[0]==0,'Only unencrypted EDL2 supported'
b=b[16:]
m=Table(b,struct.unpack_from('<I',b)[0]);g=Table(b,m.Indirect(m.Pos+m.Offset(18)))
o=g.Offset(4);nodes=g.Vector(o);ops=collections.Counter()
for i in range(g.VectorLen(o)):
 n=Table(b,g.Indirect(nodes+4*i));op=n.String(n.Pos+n.Offset(10)).decode();ops[op]+=1
assert ops and ops['Conv']>0
header=BASE/'vendor/esp-dl/esp-dl/dl/module/include/dl_module_creator.hpp'
original=header.with_suffix('.hpp.original')
if not original.exists():original.write_bytes(header.read_bytes())
s=original.read_text();registered=set(re.findall(r'this->register_module\("([^"]+)"',s))
assert set(ops)<=registered,sorted(set(ops)-registered)
s=re.sub(r'^\s*this->register_module\("([^"]+)"[^\n]+\n',lambda m:m.group(0) if m.group(1) in ops else '',s,flags=re.M)
header.write_text(s)
manifest=dict(model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),operators=dict(sorted(ops.items())),header_original_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),header_patched_sha256=hashlib.sha256(header.read_bytes()).hexdigest())
(BASE/'results/model_ops.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest))
