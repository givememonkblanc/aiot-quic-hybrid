"""Compare source SHA-256 telemetry to independently reassembled live JPEGs."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import time
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--seconds', type=float, default=120)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
feed = root/'experiments/handoff/results/cross-ap-final-mode2-03/feed.jsonl'
latest = feed.with_name('latest.jpg')
args.output.mkdir(parents=True, exist_ok=True)
diags, frames, matches = {}, {}, []
state = None
buf = bytearray()
next_seq = 0
start = time.monotonic()
complete = 0
parse_errors = 0
pending = b''
with feed.open('rb') as f:
    f.seek(0, 2)
    while time.monotonic()-start < args.seconds:
        chunk = f.read(1024*1024)
        if not chunk:
            time.sleep(.02)
            continue
        pending += chunk
        lines = pending.split(b'\n')
        pending = lines.pop()
        for line in lines:
            try:
                e = json.loads(line.decode('latin1'))
            except (ValueError, UnicodeError):
                parse_errors += 1
                continue
            if e.get('kind') != 'datagram':
                continue
            key = (e['cid'], e['gen_ts_ms'])
            data = e['payload'].encode('latin1')
            if e['track'] == 1 and b'camera_integrity' in data:
                d = json.loads(data)
                # Source ts precedes fb_get; mobility resets packet time after capture.
                # The wire gen_ts is set from that final packet time for BOTH records.
                d['source_call_ms'] = d.pop('ts')
                d['wire_gen_ts_ms'] = key[1]
                diags[key] = d
            elif e['track'] == 3:
                if data.startswith(b'\xff\xd8'):
                    state, buf, next_seq = key, bytearray(), e['seq']
                if state != key or next_seq != e['seq'] or len(buf)+len(data)>262144:
                    state, buf = None, bytearray()
                    continue
                buf.extend(data)
                next_seq = (e['seq']+1)&0xffffffff
                if buf.endswith(b'\xff\xd9'):
                    complete += 1
                    frames[key] = bytes(buf)
                    state, buf = None, bytearray()
            if key in frames and key in diags:
                jpeg, d = frames.pop(key), diags.pop(key)
                digest = hashlib.sha256(jpeg).hexdigest()
                im = Image.open(io.BytesIO(jpeg)); im.load()
                try:
                    disk_match = hashlib.sha256(latest.read_bytes()).hexdigest() == digest
                except OSError:
                    disk_match = False
                match = dict(cid=key[0], **d, received_sha256=digest,
                             source_matches_received=digest==d['sha256'],
                             length_matches=len(jpeg)==d['bytes'],
                             received_dimensions=list(im.size), latest_file_match=disk_match)
                matches.append(match)
                if len(matches)<=3:
                    (args.output/f'matched-{len(matches):02d}.jpg').write_bytes(jpeg)
                print(json.dumps(match), flush=True)
            # Retain only bounded recent diagnostic/frame history.
            while len(frames)>60: frames.pop(next(iter(frames)))
            while len(diags)>60: diags.pop(next(iter(diags)))
report = dict(seconds=time.monotonic()-start, complete_frames=complete,
              parse_errors=parse_errors, matched_count=len(matches), matches=matches)
(args.output/'verification.json').write_text(json.dumps(report, indent=2))
print(json.dumps({k:v for k,v in report.items() if k!='matches'}))
if not matches or any(not m['source_matches_received'] or not m['copy_equal'] or not m['length_matches'] for m in matches):
    raise SystemExit(1)
