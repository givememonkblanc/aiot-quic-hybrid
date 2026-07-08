#!/usr/bin/env python3
"""Capture ESP32 console output for N seconds, optionally hard-resetting first.

Replaces `idf.py monitor | tee` in the COMPARISON_PLAN procedure so captures
can run unattended: the device is hard-reset via esptool (fresh handshake per
rep) and raw UART output is written to the log file that
parse_aiot_monitor_log.py consumes.
"""
import argparse
import subprocess
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=90.0)
    parser.add_argument("--out", required=True)
    parser.add_argument("--reset", action="store_true",
                        help="hard-reset the device before capturing")
    args = parser.parse_args()

    if args.reset:
        result = subprocess.run(
            [sys.executable, "-m", "esptool", "--chip", "esp32s3",
             "--port", args.port, "--after", "hard_reset", "chip_id"],
            capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout, result.stderr, file=sys.stderr)
            return 1

    # The S3's USB-Serial-JTAG re-enumerates across resets, dropping the CDC
    # port mid-read, so keep reopening until the capture window closes.
    deadline = time.time() + args.seconds
    captured = 0
    with open(args.out, "wb") as out:
        while time.time() < deadline:
            try:
                # Open with DTR/RTS deasserted: asserting them on a
                # USB-Serial-JTAG port resets the chip, which would put the
                # capture itself into a reboot loop.
                ser = serial.Serial(timeout=1)
                ser.port = args.port
                ser.baudrate = args.baud
                ser.dtr = False
                ser.rts = False
                ser.open()
                with ser:
                    while time.time() < deadline:
                        data = ser.read(4096)
                        if data:
                            out.write(data)
                            out.flush()
                            captured += len(data)
            except (serial.SerialException, OSError):
                time.sleep(0.5)
    print(f"captured {captured} bytes over {args.seconds:.0f}s -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
