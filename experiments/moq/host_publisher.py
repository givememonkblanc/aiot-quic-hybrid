"""Native QUIC publisher using the same portable C wire codec as the MCU."""
import argparse,asyncio,ctypes,hashlib,json
from pathlib import Path
from aioquic.asyncio import QuicConnectionProtocol,connect
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import HandshakeCompleted,StreamDataReceived
from receiver import read_message,ALPN,msg,vi
class Publisher(QuicConnectionProtocol):
 def __init__(self,*a,lib,jpeg,dual=False,**kw):
  super().__init__(*a,**kw);self.lib=lib;self.jpeg=jpeg;self.buffers={};self.ready=False;self.sent=False;self.finished=False;self.dual=dual;self.finished_streams=set()
 def quic_event_received(self,e):
  if isinstance(e,HandshakeCompleted):
   assert e.alpn_protocol==ALPN
   b=ctypes.create_string_buffer(256);n=self.lib.moqt21_setup(b,256,b'localhost:14568')
   self._quic.send_stream_data(self._quic.get_next_available_stream_id(is_unidirectional=True),b.raw[:n])
  if isinstance(e,StreamDataReceived):
   if e.end_stream and self.sent:
    self.finished_streams.add(e.stream_id);self.finished=len(self.finished_streams)==(2 if self.dual else 1)
   b=self.buffers.setdefault(e.stream_id,bytearray());b.extend(e.data)
   try:t,body,n=read_message(b)
   except Exception:return
   del b[:n]
   if t==0x2f00:assert self.lib.moqt21_server_setup(body,len(body))==1;self.ready=True
   elif t==3:
    req=ctypes.c_uint64();alias=ctypes.c_uint();assert self.ready and self.lib.moqt21_subscribe_track(body,len(body),ctypes.byref(req),ctypes.byref(alias))==1
    buf=ctypes.create_string_buffer(256);n=self.lib.moqt21_subscribe_ok_track(buf,256,alias.value);self._quic.send_stream_data(e.stream_id,buf.raw[:n])
    for group in range(3):
     payload=self.jpeg if alias.value==1 else json.dumps(dict(schema=1,seq=group,backend=3,result_hex=(b'\xd1\x02'+bytes(18)).hex())).encode()
     n=self.lib.moqt21_object_header_track(buf,256,alias.value,group,len(payload));sid=self._quic.get_next_available_stream_id(is_unidirectional=True)
     self._quic.send_stream_data(sid,buf.raw[:n]);self._quic.send_stream_data(sid,payload,end_stream=True)
    self._quic.send_stream_data(e.stream_id,msg(0x0b,vi(2)+vi(3)+vi(0)),end_stream=True)
    self.sent=True
async def main(a):
 lib=ctypes.CDLL(str(a.lib.resolve()))
 lib.moqt21_setup.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_char_p];lib.moqt21_setup.restype=ctypes.c_size_t
 lib.moqt21_subscribe_ok.argtypes=[ctypes.c_void_p,ctypes.c_size_t];lib.moqt21_subscribe_ok.restype=ctypes.c_size_t
 lib.moqt21_object_header.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64,ctypes.c_size_t];lib.moqt21_object_header.restype=ctypes.c_size_t
 lib.moqt21_server_setup.argtypes=[ctypes.c_char_p,ctypes.c_size_t]
 lib.moqt21_subscribe.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_uint64)]
 lib.moqt21_subscribe_track.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint)]
 lib.moqt21_subscribe_ok_track.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint];lib.moqt21_subscribe_ok_track.restype=ctypes.c_size_t
 lib.moqt21_object_header_track.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint,ctypes.c_uint64,ctypes.c_size_t];lib.moqt21_object_header_track.restype=ctypes.c_size_t
 data=a.jpeg.read_bytes();config=QuicConfiguration(is_client=True,alpn_protocols=[ALPN],max_datagram_frame_size=1200,server_name='localhost');config.load_verify_locations(str(a.cert))
 async with connect('127.0.0.1',14568,configuration=config,create_protocol=lambda *x,**kw:Publisher(*x,lib=lib,jpeg=data,dual=a.dual,**kw)) as client:
  await asyncio.sleep(3);assert client.sent and client.finished
 print(json.dumps(dict(sent=6 if a.dual else 3,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument("--dual",action="store_true");p.add_argument('--lib',type=Path,required=True);p.add_argument('--jpeg',type=Path,required=True);p.add_argument('--cert',type=Path,required=True);asyncio.run(main(p.parse_args()))
