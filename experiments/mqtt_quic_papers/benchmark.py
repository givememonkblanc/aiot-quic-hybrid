"""Local MQTT 3.1.1 QoS 0/1 correctness/KPI baseline, not a paper replication.

Both TLS/TCP and one reliable QUIC stream proxy unchanged MQTT bytes to the
same Mosquitto broker. All endpoints are loopback. No ESP32 or host NIC changes.
"""
import argparse
import asyncio
from contextlib import asynccontextmanager
import json
import hashlib
from pathlib import Path
import platform
import resource
import socket
import ssl
import statistics
import struct
import subprocess
import tempfile
import time
import uuid

from paper_metrics import delay_metrics, allocation_metrics, metric_status

from aioquic import __version__ as aioquic_version
from aioquic.asyncio import connect, serve
from aioquic.quic.configuration import QuicConfiguration


def packet(kind, body):
    n = len(body)
    encoded = bytearray([kind])
    while True:
        byte = n % 128
        n //= 128
        encoded.append(byte | (128 if n else 0))
        if not n:
            break
    return bytes(encoded) + body


def utf8(text):
    data = text.encode()
    return struct.pack('!H', len(data)) + data


async def receive(reader):
    kind = (await reader.readexactly(1))[0]
    n, multiplier = 0, 1
    for _ in range(4):
        byte = (await reader.readexactly(1))[0]
        n += (byte & 127) * multiplier
        if not byte & 128:
            if n > 1048576:
                raise ValueError('MQTT packet exceeds benchmark bound')
            return kind, await reader.readexactly(n)
        multiplier *= 128
    raise ValueError('Malformed MQTT remaining length')


def port(udp=False):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def percentile(data, percent):
    if not data:
        return None
    values = sorted(data)
    rank = (len(values)-1)*percent/100
    low = int(rank)
    high = min(low+1, len(values)-1)
    return values[low] + (values[high]-values[low])*(rank-low)


async def bridge(reader, writer, broker_port):
    backend = None
    tasks = []
    try:
        peer, backend = await asyncio.open_connection('127.0.0.1', broker_port)

        async def pump(src, dst):
            while data := await src.read(65536):
                dst.write(data)
                await dst.drain()

        tasks = [asyncio.create_task(pump(reader, backend)), asyncio.create_task(pump(peer, writer))]
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for stream in (writer, backend):
            if stream:
                stream.close()


@asynccontextmanager
async def endpoint(transport, listener, cert):
    if transport == 'quic-stream':
        config = QuicConfiguration(is_client=True, alpn_protocols=['mqtt'], server_name='localhost')
        config.load_verify_locations(cafile=str(cert))
        async with connect('127.0.0.1', listener, configuration=config) as protocol:
            reader, writer = await protocol.create_stream()
            try:
                yield reader, writer
            finally:
                writer.close()
    else:
        config = ssl.create_default_context(cafile=str(cert))
        config.minimum_version = config.maximum_version = ssl.TLSVersion.TLSv1_3
        reader, writer = await asyncio.open_connection('127.0.0.1', listener, ssl=config, server_hostname='localhost')
        try:
            yield reader, writer
        finally:
            writer.close()
            await writer.wait_closed()


async def mqtt_connect(reader, writer, client_id):
    writer.write(packet(0x10, utf8('MQTT') + bytes([4, 2, 0, 30]) + utf8(client_id)))
    await writer.drain()
    kind, body = await receive(reader)
    if kind != 0x20 or body != b'\0\0':
        raise RuntimeError(f'CONNECT rejected: {kind}, {body!r}')


async def trial(transport, listener, cert, qos, count, size, interval_ms, run, directory):
    topic = 'paper-bench/' + uuid.uuid4().hex
    measurements, acknowledgments = [], []
    cpu_started = resource.getrusage(resource.RUSAGE_SELF)
    capture_start_epoch_s = time.time()
    started = time.perf_counter_ns()
    async with endpoint(transport, listener, cert) as (sr, sw):
        await mqtt_connect(sr, sw, 'sub-'+uuid.uuid4().hex[:12])
        sw.write(packet(0x82, b'\0\1'+utf8(topic)+bytes([qos])))
        await sw.drain()
        kind, body = await receive(sr)
        if kind != 0x90 or body != b'\0\1'+bytes([qos]):
            raise RuntimeError('SUBACK QoS not granted')

        async def subscriber():
            seen = set()
            while len(seen) < count:
                kind, body = await receive(sr)
                if kind >> 4 != 3:
                    raise RuntimeError('Expected PUBLISH at subscriber')
                offset = 2 + int.from_bytes(body[:2], 'big')
                delivered_qos = (kind >> 1) & 3
                if delivered_qos != qos:
                    raise RuntimeError('Broker delivered unexpected QoS')
                pid = body[offset:offset+2] if qos else b''
                offset += 2 if qos else 0
                payload = body[offset:]
                if len(payload) != size:
                    raise RuntimeError('Payload size changed in transit')
                seq, generated = struct.unpack('!IQ', payload[:12])
                if seq >= count:
                    raise RuntimeError('Unexpected application sequence')
                seen.add(seq)
                arrived = time.perf_counter_ns()
                measurements.append(dict(seq=seq, sent_ns=generated, received_ns=arrived,
                                          latency_ms=(arrived-generated)/1e6))
                if qos:
                    sw.write(packet(0x40, pid)); await sw.drain()

        subtask = asyncio.create_task(subscriber())
        connect_started = time.perf_counter_ns()
        try:
            async with endpoint(transport, listener, cert) as (pr, pw):
                transport_ready = time.perf_counter_ns()
                await mqtt_connect(pr, pw, 'pub-'+uuid.uuid4().hex[:12])
                mqtt_ready = time.perf_counter_ns()
                first_sent = None
                for seq in range(count):
                    now = time.perf_counter_ns()
                    if first_sent is None:
                        first_sent = now
                    payload = struct.pack('!IQ', seq, now)+bytes(size-12)
                    pid = struct.pack('!H', seq+1)
                    pw.write(packet(0x30 | (qos << 1), utf8(topic)+(pid if qos else b'')+payload))
                    await pw.drain()
                    if qos:
                        kind, body = await receive(pr)
                        if kind != 0x40 or body != pid:
                            raise RuntimeError('Missing or mismatched PUBACK')
                        acknowledgments.append((time.perf_counter_ns()-now)/1e6)
                    target = first_sent + int((seq+1)*interval_ms*1e6)
                    if seq + 1 < count:
                        await asyncio.sleep(max(0, (target-time.perf_counter_ns())/1e9))
                await asyncio.wait_for(subtask, timeout=10)
                completed = time.perf_counter_ns()
                pw.write(packet(0xe0, b'')); await pw.drain()
        finally:
            if not subtask.done():
                subtask.cancel()
            await asyncio.gather(subtask, return_exceptions=True)
        sw.write(packet(0xe0, b'')); await sw.drain()
    cpu_finished = resource.getrusage(resource.RUSAGE_SELF)
    latencies = [r['latency_ms'] for r in measurements]
    unique = len({r['seq'] for r in measurements})
    if unique != count or set(r['seq'] for r in measurements) != set(range(count)):
        raise RuntimeError('Message sequence completeness check failed')
    sample_path = directory / f'{transport}-qos{qos}-run{run}.jsonl'
    sample_path.write_text(''.join(json.dumps(r)+'\n' for r in measurements))
    # Window includes the full offered schedule and delivery drain. Payload
    # goodput excludes MQTT, TLS/QUIC, IP headers; it is not wire efficiency.
    duration = (completed-first_sent)/1e9
    return dict(transport=transport, qos=qos, run=run, sent=count, received_unique=unique,
                payload_bytes=size, interval_ms=interval_ms,
                **delay_metrics(latencies),
                duplicates=len(measurements)-unique, delivery_ratio=unique/count,
                latency_mean_ms=statistics.mean(latencies), latency_p50_ms=percentile(latencies,50),
                latency_p95_ms=percentile(latencies,95), latency_p99_ms=percentile(latencies,99),
                latency_sample_sd_ms=statistics.stdev(latencies) if count>1 else 0,
                transport_connect_ms=(transport_ready-connect_started)/1e6,
                connect_to_connack_ms=(mqtt_ready-connect_started)/1e6,
                connect_to_first_publish_ms=(first_sent-connect_started)/1e6,
                puback_p95_ms=percentile(acknowledgments,95),
                payload_goodput_bps=unique*size*8/duration, transfer_window_s=duration,
                execution_s=(completed-started)/1e9,
                publisher_execution_s=(completed-connect_started)/1e9,
                cpu_process_s=(cpu_finished.ru_utime-cpu_started.ru_utime)+
                              (cpu_finished.ru_stime-cpu_started.ru_stime),
                cpu_scope='host publisher+subscriber+both secure proxies; excludes Mosquitto',
                cpu_window='subscriber connection start through both endpoint closures',
                capture_start_epoch_s=capture_start_epoch_s, capture_end_epoch_s=time.time(),
                allocation_total_bytes=None, power_mean_w=None,
                packet_initial_to_first_publish_ms=None, compiled_footprint_bytes=None,
                energy_j=None, wire_bytes=None, wire_packets=None, device_ram_bytes=None,
                handoff_recovery_ms=None)


async def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    for name in ('benchmark.py', 'paper_metrics.py'):
        (args.output / name.replace('.py', '_source.py')).write_bytes(Path(__file__).with_name(name).read_bytes())
    with tempfile.TemporaryDirectory(prefix='mqtt-quic-paper-') as temporary:
        root = Path(temporary)
        cert, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','2',
                        '-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost',
                        '-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        key.chmod(0o600)
        broker_port, tls_port, quic_port = port(), port(), port(True)
        config = root/'mosquitto.conf'
        config.write_text(f'listener {broker_port} 127.0.0.1\nallow_anonymous true\npersistence false\n')
        with (args.output/'broker.log').open('w') as log:
            broker = subprocess.Popen(['mosquitto','-c',str(config)], stdout=log, stderr=subprocess.STDOUT)
            servers, bridge_tasks = [], set()
            try:
                for _ in range(50):
                    try:
                        _, writer = await asyncio.open_connection('127.0.0.1', broker_port)
                        writer.close(); await writer.wait_closed(); break
                    except OSError:
                        await asyncio.sleep(.1)
                else:
                    raise RuntimeError('Broker startup failed')

                def on_stream(reader, writer):
                    task = asyncio.create_task(bridge(reader, writer, broker_port))
                    bridge_tasks.add(task)
                    task.add_done_callback(bridge_tasks.discard)

                tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_3
                tls.load_cert_chain(cert, key)
                servers.append(await asyncio.start_server(on_stream, '127.0.0.1', tls_port, ssl=tls))
                quic = QuicConfiguration(is_client=False, alpn_protocols=['mqtt'])
                quic.load_cert_chain(cert, key)
                servers.append(await serve('127.0.0.1', quic_port, configuration=quic, stream_handler=on_stream))
                manifest = dict(classification='modern_mapping_loopback_smoke_NOT_paper_reproduction',
                                python=platform.python_version(), aioquic=aioquic_version,
                                openssl=ssl.OPENSSL_VERSION,
                                mosquitto=subprocess.run(['mosquitto','-h'],capture_output=True,text=True).stdout.splitlines()[0],
                                benchmark_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                                count=args.count, payload_bytes=args.payload, interval_ms=args.interval_ms,
                                repetitions=args.repetitions, tls='1.3 certificate verified',
                                schema=2, allocation_profile=args.allocation_profile,
                                listeners=dict(tcp_tls=tls_port, quic_stream=quic_port),
                                metrics_sha256=hashlib.sha256(Path(__file__).with_name('paper_metrics.py').read_bytes()).hexdigest(),
                                metric_status=metric_status(args.allocation_profile),
                                clock='same-process perf_counter_ns; no cross-device subtraction',
                                mqtt='3.1.1; clean session; QoS0 and QoS1; one in-flight QoS1 PUBLISH',
                                topology='publisher -> secure proxy -> same Mosquitto -> secure proxy -> subscriber',
                                limitations=['loopback only','no packet impairment','no ESP32 measurement',
                                             'cold handshakes only; no 0-RTT','no energy or wire capture'])
                (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
                results = []
                for run in range(args.repetitions):
                    order = [('tcp-tls',tls_port),('quic-stream',quic_port)]
                    if run % 2:
                        order.reverse()
                    for qos in (0,1):
                        for transport, listener in order:
                            async def execute():
                                return await asyncio.wait_for(trial(transport,listener,cert,qos,args.count,
                                    args.payload,args.interval_ms,run,args.output), timeout=30+args.count*args.interval_ms/1000)
                            if args.allocation_profile:
                                import memray
                                allocation_path = args.output / f'{transport}-qos{qos}-run{run}.alloc.bin'
                                with memray.Tracker(str(allocation_path), trace_python_allocators=True):
                                    result = await execute()
                                result.update(allocation_metrics(allocation_path))
                            else:
                                result = await execute()
                            result['measurement_pass'] = 'allocation' if args.allocation_profile else 'timing'
                            results.append(result)
                            (args.output/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
                            print(transport,qos,run,'delivered',result['received_unique'],
                                  'p95_ms',round(result['latency_p95_ms'],3),flush=True)
            finally:
                for server in servers:
                    server.close()
                for task in list(bridge_tasks):
                    task.cancel()
                await asyncio.gather(*bridge_tasks, return_exceptions=True)
                broker.terminate()
                try:
                    broker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    broker.kill(); broker.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--count',type=int,default=100)
    parser.add_argument('--payload',type=int,default=100)
    parser.add_argument('--interval-ms',type=float,default=10)
    parser.add_argument('--repetitions',type=int,default=3)
    parser.add_argument('--allocation-profile',action='store_true',
                        help='Separate instrumented pass; do not mix its timing with unprofiled results')
    args = parser.parse_args()
    if not (1 <= args.count <= 60000 and 12 <= args.payload <= 65536 and
            0 <= args.interval_ms <= 10000 and 1 <= args.repetitions <= 50):
        parser.error('Parameter outside benchmark bounds')
    asyncio.run(main(args))
