#!/usr/bin/env python3
"""Receiver of the frame trace of the firmware (AIOT_FRAME_TRACE_PORT): every captured JPEG at the
full input rate, over plain TCP on the local Wi-Fi, for offline replay of inference policies.

Record: 'FRM1', sequence (u32), capture time on the device clock in us (u64), length (u32), JPEG.
Each frame is written to OUT/frames/<seq>.jpg and a line to OUT/index.csv (seq, capture_us,
bytes, host arrival time).

usage: frame_trace_rx.py --port 5310 --out results/<run>/trace [--seconds 480]"""
import argparse
import socket
import struct
import time
from pathlib import Path


def read_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return bytes(buf)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=5310)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=480)
    a = p.parse_args()
    (a.out / "frames").mkdir(parents=True, exist_ok=True)
    index = open(a.out / "index.csv", "a", buffering=1)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    srv.listen(1)
    srv.settimeout(1.0)
    end = time.time() + a.seconds
    frames = 0
    while time.time() < end:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        conn.settimeout(5.0)
        try:
            while time.time() < end:
                head = read_exact(conn, 20)
                if head[:4] != b"FRM1":
                    raise ValueError("bad record")
                seq, ts, n = struct.unpack("<IQI", head[4:])
                data = read_exact(conn, n)
                (a.out / "frames" / f"{seq:07d}.jpg").write_bytes(data)
                index.write(f"{seq},{ts},{n},{time.time():.3f}\n")
                frames += 1
        except (ConnectionError, socket.timeout, ValueError) as e:
            print(f"connection ended: {e!r}, frames so far {frames}", flush=True)
        finally:
            conn.close()
    print(f"frames {frames}", flush=True)


if __name__ == "__main__":
    main()
