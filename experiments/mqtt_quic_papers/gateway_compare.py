"""Alqattaa-style topology port: MQTT/QUIC -> gateway -> MQTT/TLS 1.3.

This is a modern transparent-stream adaptation, not recovered author code.
Compares direct MQTT/TLS with the two-leg route to the same local TLS broker.
No cloud, 0-RTT, mobility, or ESP32 claim.
"""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import ssl
import subprocess
import tempfile
from aioquic.asyncio import serve
from aioquic.quic.configuration import QuicConfiguration
from benchmark import trial
from compare_author import ROOT, certs, free_port, terminate


async def main(a):
    a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False)
    (a.output/'runner_source.py').write_bytes(Path(__file__).read_bytes())
    tasks=set();errors=[];tls_sessions=[];rows=[];broker=None;server=None
    with tempfile.TemporaryDirectory(prefix='gateway-mqtt-') as temp:
        root=Path(temp);certs(root)
        cloud,quic=free_port(),free_port(True)
        cfg=root/'broker.conf'
        cfg.write_text(f'listener {cloud} 127.0.0.1\nallow_anonymous true\npersistence false\ncertfile {root}/server.pem\nkeyfile {root}/server.key\ncafile {root}/ca.pem\ntls_version tlsv1.3\n')
        try:
            with (a.output/'broker.log').open('w') as log:
                broker=subprocess.Popen(['mosquitto','-c',str(cfg)],stdout=log,stderr=subprocess.STDOUT)
            context=ssl.create_default_context(cafile=str(root/'ca.pem'))
            context.minimum_version=context.maximum_version=ssl.TLSVersion.TLSv1_3
            for _ in range(50):
                if broker.poll() is not None:raise RuntimeError('TLS broker stopped')
                try:
                    _,w=await asyncio.open_connection('127.0.0.1',cloud,ssl=context,server_hostname='localhost')
                    w.close();await w.wait_closed();break
                except OSError:await asyncio.sleep(.05)
            else:raise TimeoutError('TLS broker readiness timeout')

            async def bridge(reader,writer):
                upstream=None;pumps=[]
                try:
                    back,upstream=await asyncio.open_connection('127.0.0.1',cloud,ssl=context,server_hostname='localhost')
                    tls_sessions.append(upstream.get_extra_info('ssl_object').version())
                    async def pump(src,dst):
                        while data:=await src.read(65536):dst.write(data);await dst.drain()
                    pumps=[asyncio.create_task(pump(reader,upstream)),asyncio.create_task(pump(back,writer))]
                    done,_=await asyncio.wait(pumps,return_when=asyncio.FIRST_COMPLETED)
                    for task in done:task.result()
                except asyncio.CancelledError:raise
                except Exception as exc:errors.append(f'{type(exc).__name__}: {exc}')
                finally:
                    for task in pumps:task.cancel()
                    await asyncio.gather(*pumps,return_exceptions=True)
                    writer.close()
                    if upstream:
                        upstream.close()
                        try:await upstream.wait_closed()
                        except OSError:pass

            def incoming(reader,writer):
                task=asyncio.create_task(bridge(reader,writer));tasks.add(task);task.add_done_callback(tasks.discard)
            config=QuicConfiguration(is_client=False,alpn_protocols=['mqtt'])
            config.load_cert_chain(root/'server.pem',root/'server.key')
            server=await serve('127.0.0.1',quic,configuration=config,stream_handler=incoming)
            for run in range(a.repetitions):
                order=[('tcp-tls',cloud),('quic-stream',quic)]
                if run%2:order.reverse()
                for qos in [0,1]:
                    for transport,listener in order:
                        result=await asyncio.wait_for(trial(transport,listener,root/'ca.pem',qos,a.count,256,10,run,a.output),20)
                        result['topology']='direct-tls' if transport=='tcp-tls' else 'quic-gateway-tls'
                        rows.append(result);(a.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
                        print(result['topology'],qos,run,result['received_unique'],flush=True)
            if errors or any(v!='TLSv1.3' for v in tls_sessions):raise RuntimeError('gateway errors or wrong TLS version')
        finally:
            if server:server.close()
            for task in list(tasks):task.cancel()
            await asyncio.gather(*tasks,return_exceptions=True)
            terminate(broker)
            files=[Path(__file__),ROOT/'benchmark.py',ROOT/'paper_metrics.py']
            for path in files:(a.output/path.name).write_bytes(path.read_bytes())
            manifest=dict(classification='gateway topology semantic port, NOT original paper reproduction',
                upstream_tls_versions=tls_sessions,gateway_errors=errors,
                source_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
                limitations=['loopback synthetic 256B input','cold verified TLS; no 0-RTT','no real cloud or ESP32',
                    'direct and gateway paths have different numbers of transport legs by design',
                    'combined Python pub/sub/gateway process CPU; not comparable with standalone Rust client CPU',
                    'not the fixed replay/queue-pressure matrix'])
            (a.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--count',type=int,default=30);p.add_argument('--repetitions',type=int,default=3)
    args=p.parse_args()
    if not 1<=args.count<=1000 or not 1<=args.repetitions<=10:p.error('outside bounded test parameters')
    asyncio.run(main(args))
