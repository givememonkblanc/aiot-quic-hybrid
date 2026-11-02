#!/usr/bin/env python3
"""Writes every UDP datagram received on a port as one line '<host time> [HELPER_DET] <payload>' (the
helper ESP32's detection reports, port 5320).  usage: udp_log.py --port 5320 --out FILE --seconds 480"""
import argparse
import socket
import time

p = argparse.ArgumentParser()
p.add_argument("--port", type=int, default=5320)
p.add_argument("--out", required=True)
p.add_argument("--seconds", type=float, default=480)
a = p.parse_args()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", a.port))
s.settimeout(1.0)
end = time.time() + a.seconds
with open(a.out, "a", buffering=1) as f:
    while time.time() < end:
        try:
            data, addr = s.recvfrom(65535)
        except socket.timeout:
            continue
        text = data.decode(errors="replace").strip()
        if text.startswith("[HELPER_DET] "):
            text = text[13:]
        if text.startswith("{") and text.endswith("}"):   # the sender, to tell several helpers apart
            text = '{"src":"%s",' % addr[0] + text[1:] if len(text) > 2 else '{"src":"%s"}' % addr[0]
        f.write(f"{time.time():.3f} [HELPER_DET] {text}\n")
