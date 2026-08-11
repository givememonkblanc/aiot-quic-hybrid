#!/usr/bin/env python3
"""
Reconstruct JPEG frames from a recorded backend feed.

The receiver writes every datagram's payload into aiot_feed.jsonl. Bytes above
0x20 go in raw, so a line is not valid UTF-8 — but latin-1 maps every byte
1:1, so decoding the line as latin-1, parsing the JSON, and re-encoding the
payload as latin-1 recovers the exact bytes.

Use this to label a capture recorded before the receiver's frame archiving
existed, or to re-derive frames from a feed you kept. Frames archived live by
the receiver share one gen_ts across all chunks of a frame; in older feeds each
chunk carries its own, so the completing chunk's gen_ts names the file and
those frames cannot be joined with device AI objects (older firmware sent
none). They are still usable as reference-detector input.

Usage:
    python3 feed_to_frames.py --feed ../../backend/aiot_feed.jsonl --out frames_old/
    python3 feed_to_frames.py --feed ... --out ... --after 47000000 --before 47200000
"""

import argparse
import json
import os
import sys

TRACK_IMAGE = 3
MAX_FRAME = 262144


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--after", type=int, help="only gen_ts_ms >= this")
    ap.add_argument("--before", type=int, help="only gen_ts_ms <= this")
    ap.add_argument("--limit", type=int, help="stop after this many frames")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    buf = bytearray()
    written = 0
    skipped = 0

    with open(args.feed, "r", encoding="latin-1") as fh:
        for line in fh:
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if obj.get("kind") != "datagram" or obj.get("track") != TRACK_IMAGE:
                continue
            gen_ts = int(obj.get("gen_ts_ms", 0))
            if args.after is not None and gen_ts < args.after:
                continue
            if args.before is not None and gen_ts > args.before:
                continue

            payload = obj.get("payload", "").encode("latin-1")
            if len(buf) + len(payload) > MAX_FRAME:
                buf.clear()          # missed an EOI: drop and resync
                skipped += 1
            buf.extend(payload)

            # Trim to the Start-Of-Image marker, same as the receiver.
            soi = buf.find(b"\xff\xd8")
            if soi < 0:
                if len(buf) > 1:
                    del buf[:-1]     # keep last byte: FF may straddle chunks
                continue
            if soi > 0:
                del buf[:soi]

            eoi = buf.find(b"\xff\xd9", 2)
            if eoi < 0:
                continue
            frame_len = eoi + 2
            path = os.path.join(args.out, f"frame_{gen_ts:010d}.jpg")
            with open(path, "wb") as out:
                out.write(bytes(buf[:frame_len]))
            del buf[:frame_len]
            written += 1
            if args.limit and written >= args.limit:
                break

    print(f"wrote {written} frames to {args.out}"
          f"{f' ({skipped} resyncs)' if skipped else ''}", file=sys.stderr)


if __name__ == "__main__":
    main()
