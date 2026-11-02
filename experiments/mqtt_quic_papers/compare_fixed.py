"""Fixed generated-input comparison. Only isolated loopback processes are modified.

The pause scenario suspends application publishing. It does NOT emulate RF loss,
AP handoff, QUIC migration, RTT, or a wire-level network outage.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from compare_author import ROOT, certs, free_port, limits, process_sample, terminate
from fixed_metrics import summarize
from udp_rebind import RebindingRelay


def run_one(args, root, case, policy, scenario, qos, run):
    out=args.output/f'{case}-{policy}-{scenario}-q{qos}-r{run}'
    out.mkdir()
    tcp,tls,quic=free_port(),free_port(),free_port(True)
    strategy='control-only' if case=='tcp-tls' else case
    broker_command=[str(args.broker),'broker','--host',f'127.0.0.1:{tcp}',
        '--tls-host',f'127.0.0.1:{tls}','--quic-host',f'127.0.0.1:{quic}',
        '--tls-cert',str(root/'server.pem'),'--tls-key',str(root/'server.key'),
        '--allow-anonymous','true','--no-persistence','--storage-backend','memory',
        '--storage-dir',str(root/'storage'),'--quic-delivery-strategy',strategy]
    config=dict(url=f'mqtts://localhost:{tls}' if case=='tcp-tls' else f'quics://127.0.0.1:{quic}',
        ca=str(root/'ca.pem'),strategy=case,qos=qos,count=args.rate*args.seconds,rate=args.rate,
        payload=args.payload,budget=args.budget,deadline_ms=args.deadline_ms,drain_ms=args.drain_ms,
        policy=policy,ready=str(out/'ready.json'),output=str(out/'raw.json'),
        pause_at_ms=args.pause_at_ms,pause_ms=args.pause_ms if scenario=='pause' else 0)
    config['active_migration_at_ms']=args.pause_at_ms+args.pause_ms+20 if args.active_migration else None
    relay=None
    if args.network!='none':
        relay=RebindingRelay(quic,out/'ready.json',args.pause_at_ms if args.network=='udp-rebind' else 10**9,args.pause_ms)
        config['url']=f'quics://127.0.0.1:{relay.port}'
    if case=='h3-port':
        config['h3_port']=quic
        broker_command=[str(ROOT/'.venv/bin/python'),str(ROOT/'h3_pubsub.py'),
            '--port',str(quic),'--cert',str(root/'server.pem'),'--key',str(root/'server.key'),
            '--ready',str(out/'broker-ready')]
    (out/'config.json').write_text(json.dumps(config,indent=2)+'\n')
    client_command=[str(args.binary),str(out/'config.json')]
    if case=='h3-port':
        client_command=[str(ROOT/'.venv/bin/python'),str(ROOT/'h3_fixed_client.py'),str(out/'config.json')]
    (out/'commands.json').write_text(json.dumps(dict(broker=broker_command,client=client_command),indent=2)+'\n')
    env=dict(os.environ,TOKIO_WORKER_THREADS='2',RUST_LOG='warn')
    broker=client=None; stop=threading.Event(); watcher=None; samples=[]
    result=dict(case=case,policy=policy,scenario=scenario,qos=qos,run=run,status='failed')
    try:
        with (out/'broker.log').open('w') as log:
            broker=subprocess.Popen(broker_command,stdout=log,stderr=subprocess.STDOUT,cwd=root,env=env,preexec_fn=limits)
        for _ in range(100):
            if broker.poll() is not None:raise RuntimeError('broker stopped')
            if case=='h3-port':
                if (out/'broker-ready').exists():break
                time.sleep(.05);continue
            try:
                with socket.create_connection(('127.0.0.1',tcp),.1):break
            except OSError:time.sleep(.05)
        else:raise TimeoutError('broker readiness timeout')
        if relay:relay.start()
        with (out/'client.log').open('w') as log:
            client=subprocess.Popen(client_command,stdout=log,stderr=subprocess.STDOUT,cwd=root,env=env,preexec_fn=limits)
        began=time.monotonic()
        def monitor():
            while not stop.is_set():
                row={'elapsed_s':time.monotonic()-began}
                for name,proc in [('broker',broker),('client',client)]:
                    sample=process_sample(proc.pid)
                    if sample:row.update({name+'_'+k:v for k,v in sample.items()})
                samples.append(row);stop.wait(.02)
        watcher=threading.Thread(target=monitor);watcher.start()
        code=client.wait(timeout=args.seconds+args.drain_ms/1000+30)
        if code:raise RuntimeError(f'client exited {code}')
        raw=json.loads((out/'raw.json').read_text())
        result.update(summarize(raw,config))
        result['network']=args.network
        if relay:
            evidence=raw.get('transport_evidence',{})
            result['transport_evidence']=evidence
            result['migration_events']=raw.get('migration_events',[])
            result['same_quic_connections']=all(
                evidence.get(f'{role}_start') and evidence.get(f'{role}_end') and
                evidence[f'{role}_start']['stable_id']==evidence[f'{role}_end']['stable_id'] and
                evidence[f'{role}_end']['close_reason'] is None for role in ('publisher','subscriber'))
            result['path_validation_observed']=all(
                evidence.get(f'{role}_end',{}).get('path_challenge_received',0)>evidence.get(f'{role}_start',{}).get('path_challenge_received',0) and
                evidence.get(f'{role}_end',{}).get('path_response_sent',0)>evidence.get(f'{role}_start',{}).get('path_response_sent',0)
                for role in ('publisher','subscriber'))
            end_us=(args.pause_at_ms+args.pause_ms)*1000
            after=[r for r in raw['received'] if r['received_us']>=end_us and raw['generated'][r['seq']]['due_us']>=end_us]
            result['first_fresh_receive_after_network_resume_ms']=min((r['received_us']-end_us)/1000 for r in after) if after else None
            result['recovery_verified']=bool(after) and result['same_quic_connections'] and result['path_validation_observed']
        if result['queue_peak_bytes']>args.budget:raise AssertionError('application budget exceeded')
        result['status']='ok' if not result['invalid_payloads'] and not result['publish_errors'] and result['received_unique'] else 'protocol_failure'
    except Exception as e:
        result['error']=f'{type(e).__name__}: {e}'
    finally:
        stop.set()
        if watcher:watcher.join(timeout=2)
        for name in ['broker','client']:
            result[name+'_peak_rss_bytes']=max((s.get(name+'_rss_bytes',0) for s in samples),default=0)
        terminate(client);terminate(broker)
        if relay:
            relay_result=relay.close()
            (out/'relay.json').write_text(json.dumps(relay_result,indent=2)+'\n')
            if relay_result['error']:result.update(status='failed',relay_error=relay_result['error'])
        (out/'resources.json').write_text(json.dumps(samples)+'\n')
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--binary',type=Path,default=ROOT/'legacy_build/fixed-replay')
    p.add_argument('--broker',type=Path,default=ROOT/'legacy_build/mqttv5-author')
    p.add_argument('--seconds',type=int,default=6)
    p.add_argument('--rate',type=int,default=30)
    p.add_argument('--payload',type=int,default=262144)
    p.add_argument('--budget',type=int,default=6*1024**2)
    p.add_argument('--deadline-ms',type=int,default=2000)
    p.add_argument('--drain-ms',type=int,default=2000)
    p.add_argument('--pause-at-ms',type=int,default=1000)
    p.add_argument('--pause-ms',type=int,default=3000)
    p.add_argument('--repetitions',type=int,default=2)
    p.add_argument('--cases',nargs='+',choices=['tcp-tls','control-only','per-topic','per-publish','h3-port'],default=['tcp-tls','control-only','per-topic','per-publish'])
    p.add_argument('--policies',nargs='+',choices=['fifo','deadline','latest','amr_latest_queue_only'],default=['fifo','deadline','amr_latest_queue_only'])
    p.add_argument('--scenarios',nargs='+',choices=['normal','pause'],default=['normal','pause'])
    p.add_argument('--qos',type=int,nargs='+',choices=[0,1],default=[0])
    p.add_argument('--network',choices=['none','udp-pass','udp-rebind'],default='none')
    p.add_argument('--active-migration',action='store_true',help='Call author migrate() on publisher and subscriber 20ms after modeled network return')
    a=p.parse_args()
    if 'h3-port' in a.cases and a.qos!=[1]:
        p.error('H3 port requires --qos 1 as broker-ack profile; this is NOT MQTT QoS1')
    if a.network!='none' and (any(c in a.cases for c in ['tcp-tls','h3-port']) or a.scenarios!=['normal']):
        p.error('UDP rebind fixture requires Rust QUIC cases and --scenarios normal; do not mix application pause')
    if a.active_migration and a.network!='udp-rebind':
        p.error('active migration requires the rebind fixture')
    if not (1<=a.seconds<=60 and 1<=a.rate<=1000 and 16<=a.payload<=1024**2 and
            a.payload<=a.budget<=16*1024**2 and 1<=a.repetitions<=20 and 0<=a.pause_at_ms and
            0<=a.pause_ms and a.pause_at_ms+a.pause_ms<=a.seconds*1000 and 0<a.drain_ms<=30000):
        p.error('outside bounded workload parameters')
    a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False)
    a.binary=a.binary.resolve(strict=True);a.broker=a.broker.resolve(strict=True)
    files=[Path(__file__),ROOT/'fixed_metrics.py',ROOT/'udp_rebind.py',ROOT/'fixed_replay/client.rs',ROOT/'fixed_replay/admission_bridge.c',
           ROOT.parents[1]/'src/components/dl_qads/video_admission.c',ROOT.parents[1]/'src/components/dl_qads/include/video_admission.h']
    if 'h3-port' in a.cases:
        files += [ROOT/'h3_pubsub.py',ROOT/'h3_fixed_client.py',ROOT/'requirements.lock']
    source=a.output/'sources';source.mkdir()
    hashes={}
    for f in files:
        data=f.read_bytes();(source/f.name).write_bytes(data);hashes[str(f)]=hashlib.sha256(data).hexdigest()
    schedule=[dict(seq=i,due_us=i*1_000_000//a.rate,payload_bytes=a.payload,topic=i%4) for i in range(a.rate*a.seconds)]
    schedule_bytes=(json.dumps(schedule,separators=(',',':'))+'\n').encode()
    (a.output/'input_schedule.json').write_bytes(schedule_bytes)
    manifest=dict(workload_sha256=hashlib.sha256(schedule_bytes).hexdigest(),source_hashes=hashes,
        binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),broker_sha256=hashlib.sha256(a.broker.read_bytes()).hexdigest(),
        args={k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items()},
        classification='fixed synthetic input, actual author transport, project queue ablations, HOST ONLY',
        limitations=['application publish pause is NOT network loss or handoff',
            '6MiB application payload budget does NOT bound transport buffers or total process memory',
            'amr_latest_queue_only calls the ESP C source with unknown transport metrics; not full firmware policy',
            'no camera or AI execution; generated byte patterns contain no images',
            'latency includes generation lateness and subscriber callback scheduling',
            'per-process address-space ceiling 2GiB; separate 20ms sampled RSS'],
        author_commit='8be02fb027bc034ac2c4c14dd4bd5b491a009135')
    if 'h3-port' in a.cases:
        manifest['h3_admission_library_sha256']=hashlib.sha256((ROOT/'legacy_build/libreplay_admission.so').read_bytes()).hexdigest()
        manifest['limitations'] += ['H3 is a Python/aioquic semantic port, not original Go code or Saif QUIC tuning',
            'H3 q1 label denotes HTTP POST broker acknowledgement, NOT MQTT QoS1',
            'main H3 matrix uses server-authenticated TLS and anonymous clients to match the Rust baseline; Basic auth tested separately']
    if a.network!='none':
        manifest['network_fixture']='loopback UDP forwarding, timed bidirectional datagram drop, then new upstream source ports; NOT AP handoff'
        manifest['active_migration']=a.active_migration
        manifest['limitations'] += ['relay adds CPU and buffering; compare against udp-pass control',
            'network-return time supplied by the fixture, not an ESP32 Wi-Fi event detector',
            'path validation requires PATH_CHALLENGE received and PATH_RESPONSE sent on both original client connections']
    (a.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    rows=[]
    with tempfile.TemporaryDirectory(prefix='fixed-mqtt-') as temp:
        root=Path(temp);certs(root)
        jobs=[(case,policy,scenario,qos) for scenario in a.scenarios for policy in a.policies for qos in a.qos for case in a.cases]
        for run in range(a.repetitions):
            for case,policy,scenario,qos in (jobs if run%2==0 else list(reversed(jobs))):
                row=run_one(a,root,case,policy,scenario,qos,run);rows.append(row)
                (a.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
                print(case,policy,scenario,qos,run,row['status'],row.get('received_unique'),row.get('deadline_fraction'),flush=True)
    raise SystemExit(0 if all(r['status']=='ok' for r in rows) else 1)


if __name__=='__main__':main()
