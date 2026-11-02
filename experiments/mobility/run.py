#!/usr/bin/env python3
"""Isolated two-path Linux netns/netem MP-QUIC measurement runner."""
import argparse
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import time
import uuid
from pathlib import Path
from analyze import analyze_run

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT/'host_receiver/picoquic/picoquicdemo'
PROFILES = {
    'static': [],
    'outage': [(5, 'primary_down', 0, 10, 100, 4), (10, 'primary_restored', 0, 10, 0, 4)],
    'handover': [(5, 'primary_degraded', 0, 80, 2, 1),
                 (10, 'primary_down', 0, 10, 100, 4),
                 (15, 'primary_restored', 0, 10, 0, 4)],
}


def run(args):
    for tool in ('ip', 'tc', 'ss', 'openssl', 'setpriv'):
        if not shutil.which(tool):
            raise RuntimeError(f'missing executable: {tool}')
    privilege = [] if os.geteuid() == 0 else ['sudo', '-n']
    subprocess.run(privilege+['true'], check=True)
    binary = args.binary.resolve()
    if not binary.is_file():
        raise RuntimeError(f'missing picoquic executable: {binary}')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for d in ('client_qlog', 'server_qlog', 'client', 'server'):
        (out/d).mkdir()
    ident = uuid.uuid4().hex[:8]
    namespaces = [f'mpq-{ident}-c', f'mpq-{ident}-s']
    created, processes, handles = [], [], []
    manifest = {'schema': 1, 'mode': args.mode, 'scenario': args.scenario, 'bytes': args.bytes,
        'time_scale': args.time_scale, 'timeout_s': args.timeout, 'cc': args.cc,
        'kernel': platform.release(), 'binary': str(binary),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'git_head': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
        'git_status': subprocess.check_output(['git', '-C', str(ROOT), 'status', '--short'], text=True),
        'clock': 'CLOCK_MONOTONIC', 'events': [], 'commands': [], 'expected_events': len(PROFILES[args.scenario]),
        'topology': {'primary': '10.201.0.1 <-> 10.201.0.2, 10ms each direction, 4Mbps',
                     'secondary': '10.202.0.1 <-> 10.202.0.2, 30ms each direction, 2Mbps'},
        'scope': 'synthetic downlink bulk transfer; network impairment emulation, not RF mobility'}

    def save():
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')

    def cmd(argv, check=True):
        manifest['commands'].append(argv)
        return subprocess.run(privilege+argv, check=check, capture_output=True, text=True)

    def inside(ns, argv, check=True):
        return cmd(['ip', 'netns', 'exec', ns]+argv, check)

    def shape(path, delay, loss, rate):
        for ns in namespaces:
            inside(ns, ['tc', 'qdisc', 'replace', 'dev', f'path{path}', 'root', 'netem',
                        'limit', '1000', 'delay', f'{delay}ms', 'loss', f'{loss}%', 'rate', f'{rate}mbit'])

    def start(ns, name, argv):
        # The transport process and its artifacts retain the invoking user's identity.
        command = privilege+['ip', 'netns', 'exec', ns, 'setpriv',
            '--reuid', str(os.getuid()), '--regid', str(os.getgid()), '--clear-groups']+argv
        manifest['commands'].append(command)
        h = (out/f'{name}.log').open('w')
        handles.append(h)
        p = subprocess.Popen(command, cwd=out/name, stdout=h, stderr=subprocess.STDOUT)
        processes.append(p)
        return p

    try:
        for ns in namespaces:
            cmd(['ip', 'netns', 'add', ns])
            created.append(ns)
            inside(ns, ['ip', 'link', 'set', 'lo', 'up'])
        for path in (0, 1):
            # Create both ends directly in owned namespaces; no host routes/qdiscs touched.
            inside(namespaces[0], ['ip', 'link', 'add', f'path{path}', 'type', 'veth',
                'peer', 'name', f'path{path}', 'netns', namespaces[1]])
            for endpoint, ns in enumerate(namespaces, 1):
                inside(ns, ['ip', 'addr', 'add', f'10.{201+path}.0.{endpoint}/24', 'dev', f'path{path}'])
                inside(ns, ['ip', 'link', 'set', f'path{path}', 'up'])
        shape(0, 10, 0, 4)
        shape(1, 30, 0, 2)
        cert, key = out/'cert.pem', out/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', str(key), '-out', str(cert), '-days', '2', '-subj', '/CN=mpquic.test',
            '-addext', 'subjectAltName=DNS:mpquic.test'], check=True, capture_output=True)
        common = ['-a', 'hq-interop', '-G', args.cc, '-L']
        mp = ['-M'] if args.mode == 'multipath' else []
        server = start(namespaces[1], 'server', [str(binary), *common, *mp, '-1', '-p', '4443',
            '-c', str(cert), '-k', str(key), '-q', str(out/'server_qlog')])
        ready = False
        for _ in range(100):
            if server.poll() is not None:
                raise RuntimeError('server exited before listening; inspect server.log')
            if ':4443' in inside(namespaces[1], ['ss', '-H', '-lun']).stdout:
                ready = True
                break
            time.sleep(.05)
        if not ready:
            raise RuntimeError('server listen timeout')
        alt = []
        if mp:
            index = json.loads(inside(namespaces[0], ['ip', '-j', 'link', 'show', 'path1']).stdout)[0]['ifindex']
            alt = ['-A', f'10.202.0.1/{index}/10.202.0.2']
        t0 = time.monotonic()
        manifest['client_start_epoch_s'] = time.time()
        manifest['client_start_monotonic_s'] = t0
        client = start(namespaces[0], 'client', [str(binary), *common, *mp, *alt, '-D',
            '-n', 'mpquic.test', '-t', str(cert), '-q', str(out/'client_qlog'),
            '10.201.0.2', '4443', f'/{args.bytes}'])
        schedule = iter(PROFILES[args.scenario])
        pending = next(schedule, None)
        while client.poll() is None:
            elapsed = time.monotonic()-t0
            if elapsed > args.timeout:
                manifest['timed_out'] = True
                raise TimeoutError('client measurement timeout')
            if pending and elapsed >= pending[0]*args.time_scale:
                at, name, path, delay, loss, rate = pending
                event = {'kind': 'impairment', 'name': name, 'path': path,
                    'scheduled_relative_s': at*args.time_scale, 'started_epoch_s': time.time(),
                    'started_monotonic_s': time.monotonic(),
                    'delay_ms_each_direction': delay, 'loss_percent': loss, 'rate_mbps': rate}
                shape(path, delay, loss, rate)
                event['completed_epoch_s'] = time.time()
                event['completed_monotonic_s'] = time.monotonic()
                event['actual_relative_s'] = time.monotonic()-t0
                manifest['events'].append(event)
                save()
                pending = next(schedule, None)
            time.sleep(.02)
        manifest['client_returncode'] = client.returncode
        manifest['elapsed_s'] = time.monotonic()-t0
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        for ns, label in zip(namespaces, ('client', 'server')):
            (out/f'{label}_qdisc.json').write_text(inside(ns, ['tc', '-s', '-j', 'qdisc', 'show']).stdout)
    except BaseException as e:
        manifest['error'] = f'{type(e).__name__}: {e}'
        raise
    finally:
        # Kill only processes in the namespaces successfully created by this run.
        for ns in reversed(created):
            pids = cmd(['ip', 'netns', 'pids', ns], check=False).stdout.split()
            if pids:
                cmd(['kill', '-TERM', *pids], check=False)
        for p in processes:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pids = []
                for ns in created:
                    pids += cmd(['ip', 'netns', 'pids', ns], check=False).stdout.split()
                if pids:
                    cmd(['kill', '-KILL', *pids], check=False)
                p.wait(timeout=3)
        for ns in reversed(created):
            cmd(['ip', 'netns', 'delete', ns], check=False)
        for h in handles:
            h.close()
        save()
    summary = analyze_run(out)
    print(json.dumps(summary, indent=2))
    return 0 if summary['valid'] else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['single', 'multipath'], default='multipath')
    parser.add_argument('--scenario', choices=PROFILES, default='handover')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--binary', type=Path, default=BINARY)
    parser.add_argument('--bytes', type=int, default=32*1024*1024)
    parser.add_argument('--time-scale', type=float, default=1)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--cc', choices=['cubic', 'bbr'], default='cubic')
    args = parser.parse_args()
    if args.bytes <= 0 or args.time_scale <= 0 or args.timeout <= 0:
        parser.error('bytes, time-scale and timeout must be positive')
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    return run(args)


if __name__ == '__main__':
    raise SystemExit(main())
