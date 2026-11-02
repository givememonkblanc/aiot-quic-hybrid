#!/usr/bin/env python3
"""Collect unique live ESP32 JPEGs as one leakage-safe screw session."""
import argparse
import hashlib
import io
import json
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = ROOT / "experiments/handoff/results/cross-ap-final-mode2-03/latest.jpg"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--session", required=True, help="Physical capture session ID")
    p.add_argument("--state", required=True, choices=("empty", "screw"))
    p.add_argument("--split", required=True, choices=("train", "val", "test"))
    p.add_argument("--seconds", type=float, default=120)
    p.add_argument("--max-frames", type=int, default=300)
    p.add_argument("--min-interval", type=float, default=0.25)
    p.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    p.add_argument("--out", type=Path, default=ROOT / "datasets/screw_live")
    a = p.parse_args()
    if a.seconds <= 0 or a.max_frames <= 0 or a.min_interval < 0:
        p.error("seconds/max-frames must be positive and min-interval non-negative")

    session = a.out / a.session
    session.mkdir(parents=True, exist_ok=False)
    manifest = session / "manifest.jsonl"
    deadline = time.monotonic() + a.seconds
    seen = set()
    last_saved = 0.0
    rows = 0
    with manifest.open("x") as out:
        while time.monotonic() < deadline and rows < a.max_frames:
            try:
                stat = a.source.stat()
                data = a.source.read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                now = time.time()
                if (digest in seen or now - stat.st_mtime > 5 or
                        now - last_saved < a.min_interval):
                    time.sleep(0.05)
                    continue
                with Image.open(io.BytesIO(data)) as image:
                    image.load()
                    if image.format != "JPEG":
                        raise ValueError("not JPEG")
                    width, height = image.size
                name = f"{time.time_ns()}.jpg"
                (session / name).write_bytes(data)
                row = dict(frame=name, sha256=digest, width=width, height=height,
                           received_epoch=now, source_mtime=stat.st_mtime,
                           session=a.session, state=a.state, split=a.split)
                out.write(json.dumps(row) + "\n")
                out.flush()
                seen.add(digest)
                last_saved = now
                rows += 1
            except (OSError, ValueError):
                pass
            time.sleep(0.05)
    print(json.dumps(dict(session=a.session, state=a.state, split=a.split,
                          frames=rows, source=str(a.source))), flush=True)


if __name__ == "__main__":
    main()
