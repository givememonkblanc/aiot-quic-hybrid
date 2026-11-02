"""Run the pinned author MQTT implementation in isolated loopback processes.

No cloud provisioning, host NIC changes, firmware changes, or default ports.
Client and broker both use the selected stream strategy. Raw author results and
traces are preserved; receive counts are not silently treated as delivery ratios.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import socket
import statistics
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent


def free_port(udp=False):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (2*1024**3, 2*1024**3))
    os.nice(5)


def process_sample(pid):
    try:
        stat = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        # Fields after comm start at field 3. utime=14, stime=15, rss=24.
        return {'cpu_s': (int(stat[11])+int(stat[12]))/os.sysconf('SC_CLK_TCK'),
                'rss_bytes': int(stat[21])*os.sysconf('SC_PAGE_SIZE')}
    except (FileNotFoundError, ProcessLookupError):
        return None


def terminate(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def certs(root):
    def run(*args):
        subprocess.run(['openssl', *args], check=True, cwd=root,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
        '-keyout', 'ca.key', '-out', 'ca.pem', '-subj', '/CN=experiment-local-ca',
        '-addext', 'basicConstraints=critical,CA:TRUE')
    run('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'server.key',
        '-out', 'server.csr', '-subj', '/CN=localhost')
    (root/'extensions').write_text('subjectAltName=DNS:localhost,IP:127.0.0.1\n'
                                  'basicConstraints=critical,CA:FALSE\n'
                                  'keyUsage=digitalSignature,keyEncipherment\n'
                                  'extendedKeyUsage=serverAuth\n')
    run('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
        '-CAcreateserial', '-out', 'server.pem', '-days', '1', '-extfile', 'extensions')
    for name in ('ca.key', 'server.key'):
        (root/name).chmod(0o600)


def run_one(args, temporary, case, qos, payload, run):
    directory = args.output / f'{case}-q{qos}-b{payload}-r{run}'
    directory.mkdir()
    tcp, tls, quic = free_port(), free_port(), free_port(True)
    strategy = case if case in ('control-only', 'per-topic', 'per-publish') else 'control-only'
    command = [str(args.binary), 'broker', '--host', f'127.0.0.1:{tcp}',
               '--tls-host', f'127.0.0.1:{tls}', '--quic-host', f'127.0.0.1:{quic}',
               '--tls-cert', str(temporary/'server.pem'), '--tls-key', str(temporary/'server.key'),
               '--allow-anonymous', 'true', '--no-persistence', '--storage-backend', 'memory',
               '--storage-dir', str(temporary/'storage'), '--quic-delivery-strategy', strategy]
    env = dict(os.environ, TOKIO_WORKER_THREADS='2', RUST_LOG='warn')
    broker = client = None
    stop = threading.Event()
    samples = []
    watcher = None
    result = dict(case=case, qos=qos, payload_bytes=payload, run=run,
                  offered_rate=args.rate, topics=args.topics, mode=args.mode,
                  status='failed', host_only=True, impairment='none')
    started = time.monotonic()
    try:
        with (directory/'broker.log').open('w') as log:
            broker = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                      cwd=temporary, env=env, preexec_fn=limits)
        for _ in range(100):
            if broker.poll() is not None:
                raise RuntimeError(f'broker exited {broker.returncode}')
            try:
                with socket.create_connection(('127.0.0.1', tcp), timeout=.1):
                    break
            except OSError:
                time.sleep(.05)
        else:
            raise TimeoutError('broker readiness timeout')
        url = f'mqtts://localhost:{tls}' if case=='tcp-tls' else f'quics://127.0.0.1:{quic}'
        bench = [str(args.binary), 'bench', '--url', url, '--ca-cert', str(temporary/'ca.pem'),
                 '--mode', args.mode, '--duration', str(args.duration), '--warmup', '1',
                 '--payload-size', str(payload), '--qos', str(qos), '--topics', str(args.topics),
                 '--rate', str(args.rate), '--publishers', '1', '--subscribers', '1',
                 '--trace-dir', str(directory/'traces')]
        if case!='tcp-tls':
            bench += ['--quic-stream-strategy', strategy]
        if case=='datagram':
            bench += ['--quic-datagrams']
        (directory/'commands.json').write_text(json.dumps({'broker': command, 'client': bench}, indent=2)+'\n')
        base = process_sample(broker.pid)
        usage_start = resource.getrusage(resource.RUSAGE_CHILDREN)
        with (directory/'raw.json').open('w') as out, (directory/'client.log').open('w') as err:
            client = subprocess.Popen(bench, stdout=out, stderr=err, env=env,
                                      cwd=temporary, preexec_fn=limits)

        def monitor():
            while not stop.is_set():
                row = {'elapsed_s': time.monotonic()-started}
                for label, proc in (('broker', broker), ('client', client)):
                    sample = process_sample(proc.pid)
                    if sample:
                        row.update({label+'_'+k:v for k,v in sample.items()})
                samples.append(row)
                stop.wait(.05)

        watcher = threading.Thread(target=monitor)
        watcher.start()
        code = client.wait(timeout=args.duration+25)
        usage_end = resource.getrusage(resource.RUSAGE_CHILDREN)
        result['client_cpu_s'] = (usage_end.ru_utime+usage_end.ru_stime-
                                  usage_start.ru_utime-usage_start.ru_stime)
        if code:
            raise RuntimeError(f'client exited {code}')
        raw = json.loads((directory/'raw.json').read_text())
        stats = raw['results']
        received = stats.get('total_messages', stats.get('received', stats.get('messages', 0)))
        if not received:
            raise RuntimeError('No MQTT messages received')
        result.update(status='ok', received=received, author_metrics=stats)
        if args.mode=='hol-blocking':
            result['mean_of_topic_p95_us'] = statistics.mean(t['p95_us'] for t in stats['topics'])
        # Locate actual author message trace rather than guessing its filename.
        for candidate in (directory/'traces').glob('*.csv'):
            with candidate.open() as stream:
                reader = csv.DictReader(stream)
                if {'topic_idx', 'seq', 'latency_us'}.issubset(reader.fieldnames or []):
                    rows = list(reader)
                    result['trace_messages'] = len(rows)
                    result['trace_duplicates'] = len(rows)-len({(r['topic_idx'],r['seq']) for r in rows})
                    result['trace_latency_mean_us'] = statistics.mean(float(r['latency_us']) for r in rows) if rows else None
        match = re.search(r'published: (\d+), received: (\d+)', (directory/'client.log').read_text())
        if match:
            result['published'] = int(match[1])
            result['received_unique'] = result.get('trace_messages',received)-result.get('trace_duplicates',0)
            result['delivery_ratio'] = result['received_unique']/result['published'] if result['published'] else None
        end = process_sample(broker.pid)
        if base and end:
            result['broker_cpu_window_s'] = end['cpu_s']-base['cpu_s']
    except Exception as error:
        result['error'] = f'{type(error).__name__}: {error}'
    finally:
        stop.set()
        if watcher:
            watcher.join(timeout=2)
        for label in ('broker', 'client'):
            result[label+'_sampled_peak_rss_bytes'] = max((s.get(label+'_rss_bytes',0) for s in samples), default=0)
        result['resource_scope'] = 'broker and combined pub/sub client separately; includes warmup; 50ms RSS sampling'
        (directory/'resources.json').write_text(json.dumps(samples)+'\n')
        terminate(client)
        terminate(broker)
    (directory/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT/'legacy_build/mqttv5-lab')
    parser.add_argument('--source-dir', type=Path, default=ROOT/'upstream/mqtt-lib-lab')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--duration', type=int, default=5)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--topics', type=int, default=4)
    parser.add_argument('--rate', type=int, default=30)
    parser.add_argument('--payloads', type=int, nargs='+', default=[256])
    parser.add_argument('--qos', type=int, nargs='+', choices=[0,1], default=[0,1])
    parser.add_argument('--cases', nargs='+', choices=['tcp-tls','control-only','per-topic','per-publish','datagram'],
                        default=['tcp-tls','control-only','per-topic','per-publish'])
    parser.add_argument('--mode', choices=['hol-blocking','throughput','latency'], default='hol-blocking')
    args = parser.parse_args()
    if not (1<=args.duration<=60 and 1<=args.repetitions<=50 and 1<=args.topics<=16 and
            0<=args.rate<=10000 and all(12<=p<=65536 for p in args.payloads)):
        parser.error('Outside bounded experiment parameters')
    args.binary = args.binary.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.source_dir.resolve(strict=True)
    source_diff = subprocess.check_output(['git','diff','--','crates/mqttv5-cli/src/commands/bench_cmd.rs'],cwd=source)
    manifest = dict(classification='author_implementation_local_adaptation_NOT_full_paper_reproduction',
                    commit=subprocess.check_output(['git','rev-parse','HEAD'], cwd=source,text=True).strip(),
                    binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                    runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    cargo_lock_sha256=hashlib.sha256((source/'Cargo.lock').read_bytes()).hexdigest(),
                    source_diff_sha256=hashlib.sha256(source_diff).hexdigest(),
                    tls_verification=True, worker_threads=2, per_process_address_space_limit_bytes=2*1024**3,
                    network='loopback, no imposed loss or delay',
                    limitations=['host rather than ESP32','RSS is not cumulative allocations',
                                 'offered-rate sleep includes publish time; inspect achieved rate',
                                 'HOL sent count is parsed from author stderr; unique receives from trace'])
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (args.output/'runner_source.py').write_bytes(Path(__file__).read_bytes())
    (args.output/'author_measurement.patch').write_bytes(source_diff)
    rows=[]
    with tempfile.TemporaryDirectory(prefix='author-mqtt-compare-') as temporary:
        root=Path(temporary)
        certs(root)
        for run in range(args.repetitions):
            order=args.cases if run%2==0 else list(reversed(args.cases))
            for payload in args.payloads:
                for qos in args.qos:
                    for case in order:
                        if case=='datagram' and (qos!=0 or payload>1000):
                            continue  # Never silently fall back or compare oversized datagrams.
                        row=run_one(args,root,case,qos,payload,run)
                        rows.append(row)
                        (args.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
                        print(case,qos,payload,run,row['status'],row.get('received',row.get('error')),flush=True)
    if any(r['status']!='ok' for r in rows):
        raise SystemExit(1)


if __name__=='__main__':
    main()
