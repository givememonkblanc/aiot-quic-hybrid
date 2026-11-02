#!/usr/bin/env python3
"""Follow the local receiver feed and persist RAM diagnostics without image data."""
import argparse
import csv
import json
import time
from datetime import datetime, timezone
from pathlib import Path

FIELDS = ['received_utc', 'cid', 'seq', 'uptime_us', 'internal_free', 'internal_total',
          'internal_min', 'internal_largest', 'psram_free', 'psram_total', 'psram_min',
          'psram_largest', 'dma_free', 'dma_min', 'dma_largest', 'main_stack_min',
          'state', 'frames', 'ai_us', 'internal_used_pct', 'psram_used_pct']
QUEUE_FIELDS = ['received_utc', 'cid', 'uptime_us', 'mode', 'target_fps', 'queue_budget',
                'queue_bytes', 'queue_peak', 'tx_queue', 'tx_queue_max', 'tx_failed', 'tx_expired']


def decode(line):
    try:
        row = json.loads(line)
        if row.get('kind') != 'publish' or row.get('topic') != 'aiot/diagnostics/ram':
            return None
        payload = json.loads(row['payload'])
        if payload.get('kind') != 'ram' or payload.get('schema') != 1:
            return None
        result = {key: payload.get(key) for key in FIELDS}
        result.update({key: payload[key] for key in QUEUE_FIELDS if key in payload})
        result.update({key: payload[key] for key in ('video_policy', 'quic_rtt_ms', 'quic_cwnd', 'quic_inflight') if key in payload})
        for heap in ('internal', 'psram'):
            free, total = payload[heap + '_free'], payload[heap + '_total']
            if not isinstance(total, int) or not isinstance(free, int) or not 0 <= free <= total:
                return None
            result[heap + '_used_pct'] = round(100 * (total - free) / total, 2) if total else None
        result.update(received_utc=datetime.now(timezone.utc).isoformat(), cid=row['cid'])
        return result
    except (ValueError, KeyError, TypeError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--feed', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--from-start', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    path = args.output / 'ram.csv'
    fresh = not path.exists() or path.stat().st_size == 0
    queue_path = args.output / 'queue.csv'
    queue_fresh = not queue_path.exists() or queue_path.stat().st_size == 0
    with args.feed.open('rb') as feed, path.open('a', newline='') as out, queue_path.open('a', newline='') as queue_out:
        if not args.from_start:
            feed.seek(0, 2)
        writer = csv.DictWriter(out, fieldnames=FIELDS, extrasaction='ignore')
        queue_writer = csv.DictWriter(queue_out, fieldnames=QUEUE_FIELDS, extrasaction='ignore')
        if queue_fresh:
            queue_writer.writeheader(); queue_out.flush()
        if fresh:
            writer.writeheader(); out.flush()
        pending = b''
        identity = args.feed.stat().st_ino
        while True:
            chunk = feed.read(65536)
            if not chunk:
                stat = args.feed.stat()
                if stat.st_ino != identity:
                    raise RuntimeError('Feed replaced; restart collector for the new feed')
                if stat.st_size < feed.tell():
                    feed.seek(0); pending = b''
                time.sleep(.5)
                continue
            pending += chunk
            lines = pending.split(b'\n'); pending = lines.pop()
            for line in lines:
                result = decode(line.decode('latin1'))
                if result is None:
                    continue
                writer.writerow(result); out.flush()
                if 'queue_budget' in result:
                    queue_writer.writerow(result); queue_out.flush()
                temporary = args.output / 'latest.tmp'
                temporary.write_text(json.dumps(result, indent=2) + '\n')
                temporary.replace(args.output / 'latest.json')
                print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
