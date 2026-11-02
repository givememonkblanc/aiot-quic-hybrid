"""Fixed workload for the separately labeled H3 semantic port (broker-ack POST)."""
import asyncio
from collections import deque
import ctypes
import json
from pathlib import Path
import struct
import sys
import time
from aioquic.asyncio import connect
from h3_pubsub import Client, client_config


async def run(c):
    if c['qos']!=1:raise ValueError('H3 supports broker-ack POST only; not MQTT QoS0')
    if not 16<=c['payload']<=1024**2 or c['budget']<c['payload']:raise ValueError('invalid payload budget')
    library=ctypes.CDLL(str(Path(__file__).parent/'legacy_build/libreplay_admission.so'))
    allow=library.replay_amr_queue_allow
    allow.argtypes=[ctypes.c_uint64,ctypes.c_uint32,ctypes.c_int];allow.restype=ctypes.c_int
    queue=deque();generated=[];received=[];submitted=[]
    state=dict(bytes=0,peak=0,done=False,start=None,invalid=0)
    notify=asyncio.Event()
    def receive(topic,payload):
        if state['start'] is None:return
        if len(payload)!=c['payload']:
            state['invalid']+=1;return
        seq,due=struct.unpack('!QQ',payload[:16])
        if seq>=c['count'] or seq%4!=topic or payload[16:]!=bytes([seq%251])*(c['payload']-16):
            state['invalid']+=1;return
        now=int((time.monotonic()-state['start'])*1e6)
        received.append(dict(seq=seq,received_us=now,latency_us=now-due,qos=1))
    async with connect('127.0.0.1',c['h3_port'],configuration=client_config(c['ca']),create_protocol=Client) as sub:
        async with connect('127.0.0.1',c['h3_port'],configuration=client_config(c['ca']),create_protocol=Client) as pub:
            for topic in range(4):
                if await pub.request('PUT',str(topic))!=201:raise RuntimeError('PUT failed')
                if await sub.request('GET',str(topic),callback=lambda body,t=topic:receive(t,body))!=200:raise RuntimeError('GET failed')
            start=time.monotonic()+.750;state['start']=start
            Path(c['ready']).write_text(json.dumps({'start_epoch_s':time.time()+.750}))
            async def sleep_to(deadline):await asyncio.sleep(max(0,deadline-time.monotonic()))
            async def worker():
                while True:
                    now=time.monotonic();pause_start=start+c['pause_at_ms']/1000;pause_end=pause_start+c['pause_ms']/1000
                    if pause_start<=now<pause_end:await sleep_to(pause_end)
                    if not queue:
                        if state['done']:return
                        notify.clear();await notify.wait();continue
                    seq,payload=queue.popleft();due=generated[seq]['due_us']
                    if c['policy']!='fifo' and (time.monotonic()-start)*1e6-due>c['deadline_ms']*1000:
                        state['bytes']-=len(payload);generated[seq]['decision']='expired';continue
                    before=time.monotonic();error=None
                    try:
                        status=await pub.request('POST',str(seq%4),payload)
                        if status!=200:raise RuntimeError(f'HTTP {status}')
                    except Exception as e:error=f'{type(e).__name__}: {e}'
                    submitted.append(dict(seq=seq,at_us=int((time.monotonic()-start)*1e6),api_us=int((time.monotonic()-before)*1e6),ok=error is None,error=error))
                    state['bytes']-=len(payload)
            task=asyncio.create_task(worker())
            for seq in range(c['count']):
                due=seq*1_000_000//c['rate'];await sleep_to(start+due/1e6)
                now=int((time.monotonic()-start)*1e6)
                if c['policy']=='latest':
                    kept=deque()
                    while queue:
                        old,payload=queue.popleft()
                        if old%4==seq%4:
                            state['bytes']-=len(payload);generated[old]['decision']='superseded'
                        else:kept.append((old,payload))
                    queue.extend(kept)
                gate=c['policy']!='amr_latest_queue_only' or bool(allow(now+1,c['payload'],state['bytes']>0))
                admitted=gate and state['bytes']+c['payload']<=c['budget']
                generated.append(dict(seq=seq,due_us=due,generated_us=now,decision='admitted' if admitted else 'policy_drop' if not gate else 'budget_drop'))
                if admitted:
                    payload=struct.pack('!QQ',seq,due)+bytes([seq%251])*(c['payload']-16)
                    queue.append((seq,payload));state['bytes']+=len(payload);state['peak']=max(state['peak'],state['bytes']);notify.set()
            state['done']=True;notify.set()
            await sleep_to(start+c['count']/c['rate']+c['drain_ms']/1000)
            unfinished=not task.done();task.cancel()
            results=await asyncio.gather(task,return_exceptions=True)
            if isinstance(results[0],Exception):raise results[0]
            observation=int((time.monotonic()-start)*1e6);state['start']=None
            Path(c['output']).write_text(json.dumps(dict(generated=generated,submitted=submitted,received=received,
                invalid_payloads=state['invalid'],application_queue_peak_bytes=state['peak'],
                application_pending_bytes_at_cutoff=state['bytes'],worker_pending_at_cutoff=unfinished,
                observation_us=observation,protocol='HTTP/3 POST broker acknowledgement; NOT MQTT QoS1',
                memory_scope='application payload queue plus active POST only; excludes transport/broker buffers'),indent=2)+'\n')


if __name__=='__main__':asyncio.run(run(json.loads(Path(sys.argv[1]).read_text())))
