"""Loopback-only UDP blackout/NAT-rebinding fixture. No NIC or namespace changes."""
import json
from pathlib import Path
import selectors
import socket
import threading
import time


class RebindingRelay:
    def __init__(self, target_port, ready_file, at_ms, outage_ms):
        self.target=('127.0.0.1',target_port);self.ready_file=Path(ready_file)
        self.at=at_ms/1000;self.duration=outage_ms/1000
        self.front=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.front.bind(('127.0.0.1',0));self.front.setblocking(False)
        self.port=self.front.getsockname()[1]
        self.stop=threading.Event();self.thread=None
        self.events=[];self.counts={'forwarded':0,'blackout_dropped':0,'send_errors':0}
        self.error=None

    def start(self):
        self.thread=threading.Thread(target=self._run,name='loopback-nat-rebind');self.thread.start()

    def close(self):
        self.stop.set()
        if self.thread:self.thread.join(timeout=3)
        else:self.front.close()
        if self.thread and self.thread.is_alive():raise RuntimeError('relay failed to stop')
        return dict(events=self.events,counts=self.counts,error=self.error,
            scope='actual QUIC datagram drop and upstream UDP tuple rebind; NOT AP handoff')

    def _run(self):
        selector=selectors.DefaultSelector();selector.register(self.front,selectors.EVENT_READ,None)
        peers={};start=None;began=False;rebound=False
        def endpoint(client):
            sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
            sock.bind(('127.0.0.1',0));sock.connect(self.target);sock.setblocking(False)
            selector.register(sock,selectors.EVENT_READ,client);return sock
        try:
            while not self.stop.is_set():
                if start is None and self.ready_file.exists():
                    try:
                        value=json.loads(self.ready_file.read_text())
                        start=time.monotonic()+value['start_epoch_s']-time.time()
                    except (ValueError,KeyError):pass  # marker may be observed mid-write
                now=time.monotonic();elapsed=now-start if start is not None else None
                blackout=elapsed is not None and self.at<=elapsed<self.at+self.duration
                if blackout and not began:
                    began=True;self.events.append(dict(kind='blackout_start',elapsed_s=elapsed))
                if elapsed is not None and elapsed>=self.at+self.duration and not rebound:
                    changes=[]
                    for client,old in list(peers.items()):
                        new=endpoint(client)  # allocate before closing old: port cannot be reused
                        changes.append(dict(old_port=old.getsockname()[1],new_port=new.getsockname()[1]))
                        selector.unregister(old);old.close();peers[client]=new
                    rebound=True;self.events.append(dict(kind='upstream_rebind',elapsed_s=elapsed,flows=changes))
                for key,_ in selector.select(.002):
                    if key.fileobj.fileno()<0:continue
                    try:
                        if key.data is None:
                            data,client=self.front.recvfrom(65535)
                            if client not in peers:peers[client]=endpoint(client)
                            destination=peers[client]
                            if blackout:self.counts['blackout_dropped']+=1;continue
                            destination.send(data)
                        else:
                            data=key.fileobj.recv(65535)
                            if blackout:self.counts['blackout_dropped']+=1;continue
                            self.front.sendto(data,key.data)
                        self.counts['forwarded']+=1
                    except (BlockingIOError,ConnectionRefusedError):self.counts['send_errors']+=1
        except Exception as exc:self.error=f'{type(exc).__name__}: {exc}'
        finally:
            selector.close();self.front.close()
            for sock in peers.values():sock.close()
