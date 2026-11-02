#!/usr/bin/env python3
"""Subscriber on another machine (wan_sub.py starts it over ssh): runs receiver_draft21.py and
writes its events to stdout as they are recorded, so that the host follows the run.

Before and after the run it measures the offset of its monotonic clock to that of the host:
it sends UDP probes to the clock echo of the host (wan_sub.ClockEcho), which answers with its
own monotonic time. The probe with the smallest round trip gives
    offset = host time - (send + receive) / 2,   uncertainty = round trip / 2,
and the host maps the times of the events with it.

usage: remote_sub.py --clock HOST:PORT --out DIR [--program P] -- <arguments of P without --out>
  P: receiver_draft21.py (MOQT, default) or mqtt_subscriber.py (MQTT)"""
import argparse
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path


def clock(host, port, probes=20):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    best = None
    for i in range(probes):
        t0 = time.monotonic()
        s.sendto(struct.pack("!Id", i, t0), (host, port))
        try:
            data, _ = s.recvfrom(64)
        except socket.timeout:
            continue
        t1 = time.monotonic()
        k, h = struct.unpack("!Id", data[:12])
        if k != i:
            continue
        rtt = t1 - t0
        if best is None or rtt < best[0]:
            best = (rtt, h - (t0 + t1) / 2)
        time.sleep(0.02)
    s.close()
    return None if best is None else dict(offset=best[1], uncertainty=best[0] / 2)


def emit(record):
    sys.stdout.write(json.dumps(record) + "\n")
    sys.stdout.flush()


def main():
    argv = sys.argv[1:]
    rest = argv[argv.index("--") + 1:] if "--" in argv else []
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--clock", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--program", default="receiver_draft21.py", choices=["receiver_draft21.py", "mqtt_subscriber.py"])
    a = p.parse_args(argv[:argv.index("--")] if "--" in argv else argv)
    host, port = a.clock.rsplit(":", 1)
    emit(dict(t=time.monotonic(), event="clock", phase="start", **(clock(host, int(port)) or {})))
    a.out.mkdir(parents=True, exist_ok=True)
    log = a.out / "events.jsonl"
    log.unlink(missing_ok=True)
    here = Path(__file__).resolve().parent
    proc = subprocess.Popen([sys.executable, "-u", str(here / a.program), "--out", str(a.out), *rest],
                            stdout=(a.out / "receiver.log").open("w"), stderr=subprocess.STDOUT)
    pos = 0
    try:
        while True:
            running = proc.poll() is None
            if log.exists():
                with log.open() as f:
                    f.seek(pos)
                    while True:
                        line = f.readline()
                        if not line.endswith("\n"):
                            break
                        pos = f.tell()
                        sys.stdout.write(line)
                sys.stdout.flush()
            if not running:
                break
            time.sleep(0.05)
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(5)
    emit(dict(t=time.monotonic(), event="clock", phase="end", **(clock(host, int(port)) or {})))


if __name__ == "__main__":
    main()
