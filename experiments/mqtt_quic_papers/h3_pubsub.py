"""Modern semantic port of Saif's H3 method mapping; NOT the original Go code.

PUT/HEAD/DELETE manage topics; GET streams length-prefixed events; POST waits
for broker HTTP 200. This ack is NOT proof of subscriber delivery. TLS server
verification and optional connection-cached Basic authentication are supported.
"""
import argparse
import asyncio
import base64
import hmac
import json
from pathlib import Path
import struct

from aioquic.asyncio import QuicConnectionProtocol, connect, serve
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import HeadersReceived, DataReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated

MAX_BODY=1024*1024


class Broker(QuicConnectionProtocol):
    def __init__(self,*args,topics,credential=None,**kwargs):
        super().__init__(*args,**kwargs)
        self.http=H3Connection(self._quic)
        self.topics=topics;self.credential=credential;self.authenticated=credential is None
        self.requests={};self.subscriptions=set()

    def response(self,stream,status,body=b'',end=True):
        self.http.send_headers(stream,[(b':status',str(status).encode()),(b'content-type',b'application/octet-stream')],end_stream=end and not body)
        if body:self.http.send_data(stream,body,end_stream=end)
        self.transmit()

    def cleanup(self):
        for topic,stream in self.subscriptions:
            self.topics.get(topic,set()).discard((self,stream))
        self.subscriptions.clear();self.requests.clear()

    def quic_event_received(self,event):
        if isinstance(event,ConnectionTerminated):
            self.cleanup();return
        for ev in self.http.handle_event(event):
            if isinstance(ev,HeadersReceived):
                h=dict(ev.headers);path=h.get(b':path',b'')
                authorized=self.authenticated or (self.credential is not None and hmac.compare_digest(h.get(b'authorization',b''),self.credential))
                if not authorized:self.response(ev.stream_id,401);continue
                self.authenticated=True
                if not path.startswith(b'/topics/') or len(path)>128:
                    self.response(ev.stream_id,400);continue
                method=h.get(b':method',b'');topic=path
                if method==b'POST':
                    self.requests[ev.stream_id]=(topic,bytearray())
                    if ev.stream_ended:self.publish(ev.stream_id)
                elif method==b'PUT':
                    self.topics.setdefault(topic,set());self.response(ev.stream_id,201)
                elif method==b'HEAD':self.response(ev.stream_id,200 if topic in self.topics else 404)
                elif method==b'GET':
                    if topic not in self.topics:self.response(ev.stream_id,404)
                    else:
                        self.topics[topic].add((self,ev.stream_id));self.subscriptions.add((topic,ev.stream_id))
                        self.response(ev.stream_id,200,end=False)
                elif method==b'DELETE':
                    for peer,stream in self.topics.pop(topic,set()):
                        peer.subscriptions.discard((topic,stream))
                        peer.http.send_data(stream,b'',end_stream=True);peer.transmit()
                    self.response(ev.stream_id,204)
                else:self.response(ev.stream_id,405)
            elif isinstance(ev,DataReceived) and ev.stream_id in self.requests:
                self.requests[ev.stream_id][1].extend(ev.data)
                if len(self.requests[ev.stream_id][1])>MAX_BODY:
                    self.requests.pop(ev.stream_id);self.response(ev.stream_id,413)
                elif ev.stream_ended:self.publish(ev.stream_id)

    def publish(self,stream):
        topic,body=self.requests.pop(stream)
        if topic not in self.topics:self.response(stream,404);return
        framed=struct.pack('!I',len(body))+body
        for peer,sid in list(self.topics[topic]):
            peer.http.send_data(sid,framed,end_stream=False);peer.transmit()
        self.response(stream,200)


class Client(QuicConnectionProtocol):
    def __init__(self,*args,credential=None,**kwargs):
        super().__init__(*args,**kwargs)
        self.http=H3Connection(self._quic);self.credential=credential;self.authenticated=False
        self.pending={};self.callbacks={};self.buffers={}

    def quic_event_received(self,event):
        if isinstance(event,ConnectionTerminated):
            for fut in self.pending.values():
                if not fut.done():fut.set_exception(ConnectionError('H3 connection terminated'))
            self.pending.clear();self.callbacks.clear();self.buffers.clear();return
        for ev in self.http.handle_event(event):
            if isinstance(ev,HeadersReceived):
                status=int(dict(ev.headers).get(b':status',b'0'))
                if 200<=status<300:self.authenticated=True
                else:
                    self.callbacks.pop(ev.stream_id,None);self.buffers.pop(ev.stream_id,None)
                future=self.pending.pop(ev.stream_id,None)
                if future and not future.done():future.set_result(status)
            elif isinstance(ev,DataReceived) and ev.stream_id in self.callbacks:
                buf=self.buffers[ev.stream_id];buf.extend(ev.data)
                while len(buf)>=4:
                    size=struct.unpack('!I',buf[:4])[0]
                    if size>MAX_BODY:raise ValueError('oversized H3 event')
                    if len(buf)<size+4:break
                    body=bytes(buf[4:4+size]);del buf[:4+size]
                    self.callbacks[ev.stream_id](body)
                if ev.stream_ended:
                    self.callbacks.pop(ev.stream_id,None);self.buffers.pop(ev.stream_id,None)

    async def request(self,method,topic,body=b'',callback=None):
        stream=self._quic.get_next_available_stream_id()
        future=asyncio.get_running_loop().create_future();self.pending[stream]=future
        if callback:self.callbacks[stream]=callback;self.buffers[stream]=bytearray()
        headers=[(b':method',method.encode()),(b':scheme',b'https'),(b':authority',b'localhost'),(b':path',f'/topics/{topic}'.encode())]
        if self.credential is not None and not self.authenticated:headers.append((b'authorization',self.credential))
        self.http.send_headers(stream,headers,end_stream=not body)
        if body:self.http.send_data(stream,body,end_stream=True)
        self.transmit()
        try:return await asyncio.wait_for(future,10)
        finally:self.pending.pop(stream,None)


def client_config(ca):
    c=QuicConfiguration(is_client=True,alpn_protocols=H3_ALPN,server_name='localhost',max_data=16*1024**2,max_stream_data=2*1024**2)
    c.load_verify_locations(cafile=str(ca));return c


def basic(value):return b'Basic '+base64.b64encode(value.encode()) if value else None


async def server_main(a):
    config=QuicConfiguration(is_client=False,alpn_protocols=H3_ALPN,max_data=16*1024**2,max_stream_data=2*1024**2)
    config.load_cert_chain(a.cert,a.key)
    credential=basic(Path(a.auth_file).read_text().strip()) if a.auth_file else None
    topics={}
    server=await serve('127.0.0.1',a.port,configuration=config,
        create_protocol=lambda *args,**kw:Broker(*args,topics=topics,credential=credential,**kw))
    Path(a.ready).write_text('ready\n')
    try:await asyncio.Event().wait()
    finally:server.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',type=int,required=True);p.add_argument('--cert',required=True);p.add_argument('--key',required=True)
    p.add_argument('--ready',required=True);p.add_argument('--auth-file')
    asyncio.run(server_main(p.parse_args()))
