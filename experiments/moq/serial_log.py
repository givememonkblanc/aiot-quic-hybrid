#!/usr/bin/env python3
"""Appends a device's serial output to a file with host timestamps until terminated.

usage: serial_log.py PORT FILE"""

import os
import select
import sys
import time
import tty


def main(port, path):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    pending = b""
    with open(path, "a") as out:
        while True:
            if not select.select([fd], [], [], 0.5)[0]:
                continue
            try:
                pending += os.read(fd, 65536)
            except OSError:
                time.sleep(0.5)
                continue
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                out.write(f"{time.time():.3f} {line.decode(errors='replace').rstrip()}\n")
                out.flush()


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
