"""Bounded CRC-checked deferred journal receiver. Never accepts missing pages."""
import json,struct,zlib
class AuditReader:
 def __init__(self,send,inject_retry=False):
  self.send=send;self.begin=None;self.pages={};self.complete=False;self.lines=[];self.raw=None;self.duplicates=0;self.inject_retry=inject_retry;self.injected=False;self.invalid_frames=0
 def consume(self,line):
  if not line.startswith('[AUDIT_'):return False
  try:
   if line.startswith('[AUDIT_ERROR]'):raise RuntimeError(line)
   tag,data=line.split(' ',1);v=json.loads(data)
   if tag=='[AUDIT_BEGIN]':
    assert v['schema']==1 and 0<=v['bytes']<=v['capacity']<=384*1024
    assert v['pages']==(v['bytes']+255)//256
    assert not(v['overflow'] or v['truncated'] or v['late']),'device journal loss'
    if self.begin:assert self.begin==v,'journal changed during retry'
    self.begin=v;self.send(b'A -1\n')
   elif tag=='[AUDIT_CHUNK]':
    assert self.begin is not None
    i=v['i'];assert 0<=i<self.begin['pages'] and v['o']==i*256
    b=bytes.fromhex(v['hex']);assert len(b)==v['n']==min(256,self.begin['bytes']-i*256)
    assert zlib.crc32(b)==v['crc'],'page CRC mismatch'
    if i in self.pages:
     assert self.pages[i]==b,'page changed during retry';self.duplicates+=1
    self.pages[i]=b
    if self.inject_retry and not self.injected:self.injected=True
    else:self.send(f'A {i}\n'.encode())
   elif tag=='[AUDIT_END]':
    assert self.begin and len(self.pages)==self.begin['pages'],'missing journal page'
    b=b''.join(self.pages[i] for i in range(self.begin['pages']))
    assert len(b)==v['bytes']==self.begin['bytes']
    assert zlib.crc32(b)==v['crc']==self.begin['crc'] and v['late']==0
    lines=[];p=0
    while p<len(b):
     assert p+2<=len(b),'truncated record length'
     n=struct.unpack_from('<H',b,p)[0];p+=2;assert 0<n<512 and p+n<=len(b),'invalid record length'
     lines.append(b[p:p+n].decode('utf-8').strip());p+=n
    assert len(lines)==self.begin['records'],'record count mismatch'
    self.lines=lines;self.raw=b;self.complete=True;self.send(b'A -2\n')
   else:raise ValueError('unknown audit message')
  except (ValueError,KeyError,AssertionError) as e:
   # Ignore damaged header/page and await firmware retry. Invalid END will never
   # be ACKed or become a successful trial. All attempts remain in serial.log.
   self.last_error=str(e);self.invalid_frames+=1
  return True
