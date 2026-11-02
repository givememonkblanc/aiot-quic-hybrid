#!/usr/bin/env python3
"""Receives the console copy of a device over UDP and writes it as serial_log.py does.

Each datagram is "NL1 <boot> <seq>\\n" followed by whole lines. Every line is written with
the host time of arrival. Lines of a boot other than the first one seen are kept as well, so
a restart is visible. A gap in the sequence is written as a [NETLOG_GAP] line.

usage: netlog_receiver.py FILE [--port 14570] [--device IP]"""

import argparse
import socket
import time


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file")
    p.add_argument("--port", type=int, default=14570)
    p.add_argument("--device", help="accept datagrams from this address only")
    a = p.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind(("0.0.0.0", a.port))
    expected = {}
    with open(a.file, "a") as out:
        while True:
            data, (host, _) = s.recvfrom(65536)
            now = time.time()
            if a.device and host != a.device:
                continue
            head, _, body = data.partition(b"\n")
            fields = head.split()
            if len(fields) != 3 or fields[0] != b"NL1":
                continue
            boot, seq = fields[1].decode(), int(fields[2])
            if boot in expected and seq != expected[boot]:
                out.write(f"{now:.3f} [NETLOG_GAP] {{\"boot\":\"{boot}\",\"expected\":{expected[boot]},\"received\":{seq}}}\n")
            expected[boot] = seq + 1
            for line in body.split(b"\n"):
                if line:
                    out.write(f"{now:.3f} {line.decode(errors='replace').rstrip()}\n")
            out.flush()


if __name__ == "__main__":
    main()
