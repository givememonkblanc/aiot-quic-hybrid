"""Read-only sampling of receiver originals for resolution comparison."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import statistics
import time

from PIL import Image, ImageStat

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--width", type=int, required=True)
parser.add_argument("--height", type=int, required=True)
parser.add_argument("--seconds", type=float, default=20)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = root / "experiments/handoff/results/cross-ap-final-mode2-03/latest.jpg"
args.output.mkdir(parents=True, exist_ok=True)
records = []
seen = set()
start = time.monotonic()
while time.monotonic() - start < args.seconds:
    try:
        # Atomic receiver rename means this descriptor stays on one frame.
        with source.open("rb") as f:
            import os
            st = os.fstat(f.fileno())
            data = f.read()
        if st.st_mtime_ns in seen:
            time.sleep(.04)
            continue
        image = Image.open(io.BytesIO(data))
        image.load()
        if image.size != (args.width, args.height):
            raise ValueError(f"Unexpected resolution: {image.size}")
        if time.time() - st.st_mtime > 3:
            raise ValueError("Stale source frame")
        seen.add(st.st_mtime_ns)
        record = dict(mtime_ns=st.st_mtime_ns, bytes=len(data),
                      sha256=hashlib.sha256(data).hexdigest(),
                      mean_luma=ImageStat.Stat(image.convert("L")).mean[0])
        # Store unmodified JPEGs, not resampled/annotated images.
        if len(records) < 10:
            (args.output / f"frame-{len(records):02d}.jpg").write_bytes(data)
        records.append(record)
    except OSError:
        pass
    time.sleep(.04)
elapsed = time.monotonic() - start
if not records:
    raise SystemExit("No fresh frames sampled")
report = dict(width=args.width, height=args.height, seconds=elapsed,
              frame_updates=len(records), received_fps=len(records)/elapsed,
              mean_jpeg_bytes=statistics.mean(r["bytes"] for r in records),
              mean_luma=statistics.mean(r["mean_luma"] for r in records),
              records=records)
(args.output / "samples.json").write_text(json.dumps(report, indent=2))
print(json.dumps({k: v for k, v in report.items() if k != "records"}))
