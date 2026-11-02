#!/usr/bin/env python3
"""Capture a real ESP32 trial, optionally applying timed serial commands.
Plan JSON: [{"at_s": 10, "commands": ["mode 2", "set wifi.ssid ...",
"set wifi.pass ...", "handoff"]}]. Keep plans with credentials outside Git.
"""
import argparse
import json
import os
import re
import subprocess
import time
import termios
import tty
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--serial', default='/dev/ttyACM0')
    p.add_argument('--duration', type=float, default=60)
    p.add_argument('--server-host', help='Reachable stable server IP to set in RAM, not saved to NVS')
    p.add_argument('--mode', choices=['0', '1', '2'], default='2')
    p.add_argument('--plan', type=Path)
    p.add_argument('--reboot', action='store_true', help='Reboot into a fresh queue before each trial')
    p.add_argument('--no-server', action='store_true')
    p.add_argument('--keep-server', action='store_true', help='Leave receiver running after a successful capture')
    p.add_argument('--binary', type=Path, default=ROOT/'build_handoff_receiver/picoquicdemo')
    args = p.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    plan = json.loads(args.plan.read_text()) if args.plan else []
    secrets = [c[len('set wifi.pass '):] for e in plan for c in e['commands'] if c.startswith('set wifi.pass ')]
    def sanitize(text):
        for secret in secrets:
            text = text.replace(secret, '[redacted]')
        return text
    manifest = {'started_epoch_s': time.time(), 'duration_s': args.duration, 'mode': int(args.mode),
        'serial': args.serial, 'server_host': args.server_host, 'reboot': args.reboot, 'commands': [],
        'firmware_sha256': None, 'scope': 'ESP32 real device; serial time is host observation time'}
    import hashlib
    firmware = ROOT/'build_mobility/aiot_quic.bin'
    if firmware.exists(): manifest['local_build_sha256'] = hashlib.sha256(firmware.read_bytes()).hexdigest()
    if not args.no_server:
        manifest['receiver_sha256'] = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    # The local binary hash does not establish which image is flashed; retain boot logs separately.
    server = None
    handles = []
    fd = None
    completed = False
    try:
        if not args.no_server:
            env = os.environ.copy()
            for key, name in [('AIOT_FEED_LOG', 'feed.jsonl'), ('AIOT_DGRAM_LOG', 'datagrams.csv'),
                              ('AIOT_MQTT_LOG', 'mqtt.csv'), ('AIOT_IMAGE_OUT', 'latest.jpg')]:
                env[key] = str(args.out/name)
            h = (args.out/'server.log').open('w'); handles.append(h)
            server = subprocess.Popen(['stdbuf', '-oL', str(args.binary.resolve()),
                '-l', str(args.out/'packets.log'), '-L', '-p', '14567', '-a', 'mqtt', '-c', str(ROOT/'host_receiver/certs/cert.pem'),
                '-k', str(ROOT/'host_receiver/certs/key.pem')], cwd=args.out, env=env,
                stdout=h, stderr=subprocess.STDOUT, start_new_session=True)
            manifest['receiver_pid'] = server.pid
        # Raw USB CDC console; leave control lines alone and disable close-time hangup.
        fd = os.open(args.serial, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd); attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIFLUSH)
        raw = (args.out/'serial.log').open('w'); handles.append(raw)
        timed = (args.out/'serial_timed.jsonl').open('w'); handles.append(timed)
        start = time.monotonic(); pending = b''; steps = list(sorted(plan, key=lambda e: e['at_s']))
        initial = [f'mode {args.mode}']
        if args.server_host: initial += [f'set server.host {args.server_host}']
        steps.insert(0, {'at_s': 3 if args.reboot else 0, 'commands': initial})
        if args.reboot: steps.insert(0, {'at_s': 0, 'commands': ['reboot']})
        while time.monotonic()-start < args.duration:
            if server is not None and server.poll() is not None:
                raise RuntimeError(f'receiver exited: {server.returncode}; inspect server.log')
            if steps and time.monotonic()-start >= steps[0]['at_s']:
                step = steps.pop(0)
                for command in step['commands']:
                    os.write(fd, ('\n'+command+'\n').encode())
                    manifest['commands'].append({'command': sanitize(command), 'host_monotonic_s': time.monotonic()})
                    time.sleep(.1)
            try:
                pending += os.read(fd, 65536)
            except BlockingIOError:
                pass
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                text = sanitize(re.sub(r'\x1b\[[0-9;]*m', '', line.decode('utf-8', 'replace')).rstrip('\r'))
                raw.write(text+'\n')
                timed.write(json.dumps({'host_monotonic_s': time.monotonic(), 'host_epoch_s': time.time(), 'line': text})+'\n')
                if ('event=' in text and 'event=generate' not in text and 'event=enqueue' not in text) or '[ERR]' in text or '[OK]' in text:
                    print(text, flush=True)
            raw.flush(); timed.flush()
            time.sleep(.03)
        completed = True
    finally:
        if fd is not None: os.close(fd)
        manifest['receiver_left_running'] = bool(server and completed and args.keep_server)
        if server and not (completed and args.keep_server):
            server.terminate()
            try: server.wait(timeout=5)
            except subprocess.TimeoutExpired: server.kill(); server.wait()
        if server and completed and args.keep_server:
            feed = args.out/'feed.jsonl'
            if feed.exists():
                data = feed.read_bytes()
                boundary = data.rfind(b'\n')
                (args.out/'feed_snapshot.jsonl').write_bytes(data[:boundary+1])
        for h in handles: h.close()
        (args.out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')

if __name__ == '__main__': main()
