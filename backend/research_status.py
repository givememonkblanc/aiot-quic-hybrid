"""Latest values and receiver-clock timing for the single-device monitor."""
import json
import statistics
import struct
import threading
import time
from collections import OrderedDict, deque
from pathlib import Path


class ReceiverStatus:
    KINDS = ('image', 'ai', 'telemetry', 'critical', 'memory')

    def __init__(self, path):
        self.path = Path(path)
        self.lock = threading.Lock()
        self.latest = {}
        self.intervals = {k: deque(maxlen=12) for k in self.KINDS}
        self.frames = OrderedDict()
        self.frame_key = None
        self.frame_data = bytearray()
        self.next_seq = None
        self.error = None

    def accept(self, r):
        if not isinstance(r, dict) or type(r.get('t')) is not int:
            return
        t = r['t'] / 1e6
        kind = None
        data = None
        seq = r.get('seq')
        generation = r.get('gen_ts_ms')
        key = (r.get('cid'), generation)
        if r.get('kind') == 'datagram' and r.get('track') in (2, 3):
            try:
                raw = r.get('payload', '').encode('latin1')
            except (UnicodeError, AttributeError):
                return
            if r['track'] == 3:
                if raw.startswith(b'\xff\xd8'):
                    self.frame_key, self.frame_data, self.next_seq = key, bytearray(), seq
                if key != self.frame_key or seq != self.next_seq or len(self.frame_data) + len(raw) > 262144:
                    self.frame_key, self.frame_data = None, bytearray()
                    return
                self.frame_data.extend(raw)
                self.next_seq = (seq + 1) & 0xffffffff
                if not self.frame_data.endswith(b'\xff\xd9'):
                    return
                kind = 'image'
                data = {'bytes': len(self.frame_data), 'frame_ms': generation}
                self.frames[key] = t
                while len(self.frames) > 128:
                    self.frames.popitem(last=False)
                self.frame_key, self.frame_data = None, bytearray()
            else:
                if len(raw) < 20 or raw[0:2] != b'\xd1\x02' or len(raw) < 20 + raw[3]*6:
                    return
                sw, sh, iw, ih, source = struct.unpack_from('<HHHHI', raw, 4)
                kind = 'ai'
                data = {'det_count': raw[3], 'raw_score': raw[16], 'in_w': iw,
                        'in_h': ih, 'source_seq': source, 'frame_ms': generation}
        elif r.get('kind') in ('publish', 'datagram'):
            try:
                data = json.loads(r.get('payload', ''))
            except (ValueError, TypeError):
                return
            if not isinstance(data, dict):
                return
            if data.get('kind') == 'ram':
                kind = 'memory'
                generation = data.get('uptime_us', 0) / 1000
            elif type(data.get('critical')) is bool:
                kind = 'critical' if data['critical'] else 'telemetry'
                generation = data.get('gen_us', 0) / 1000
            seq = data.get('seq')
        if kind is None:
            return
        old = self.latest.get(kind)
        if old and old['cid'] == r.get('cid') and t > old['received_s']:
            self.intervals[kind].append(t-old['received_s'])
        elif old and old['cid'] != r.get('cid'):
            self.intervals[kind].clear()
        self.latest[kind] = dict(received_s=t, cid=r.get('cid'), seq=seq,
                                 generation_ms=generation, data=data)

    def snapshot(self, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            rows = {}
            for kind in self.KINDS:
                item = self.latest.get(kind)
                if not item:
                    rows[kind] = {'state': 'waiting', 'age_s': None}
                    continue
                interval = statistics.median(self.intervals[kind]) if self.intervals[kind] else None
                threshold = max(3, 3 * interval) if interval else 20
                age = max(0, now-item['received_s'])
                rows[kind] = dict(item, age_s=round(age, 2), interval_s=round(interval, 2) if interval else None,
                                  threshold_s=round(threshold, 2),
                                  state=('event' if kind == 'critical' else 'late' if age > threshold else 'recent'))
            ai = self.latest.get('ai')
            camera = self.latest.get('image')
            alignment = {'same_frame_receive_delta_ms': None, 'source_gap_ms': None}
            if ai:
                frame_time = self.frames.get((ai['cid'], ai['generation_ms']))
                if frame_time is not None:
                    alignment['same_frame_receive_delta_ms'] = round((ai['received_s'] - frame_time)*1000, 1)
                if camera and camera['cid'] == ai['cid']:
                    alignment['source_gap_ms'] = round(camera['generation_ms']-ai['generation_ms'], 1)
            return {'rows': rows, 'alignment': alignment, 'error': self.error}

    def run(self):
        position = None
        identity = None
        pending = b''
        while True:
            try:
                with self.path.open('rb') as f:
                    st = self.path.stat()
                    ident = (st.st_dev, st.st_ino)
                    if identity != ident or position is None or st.st_size < position:
                        with self.lock:
                            self.latest.clear()
                            self.frames.clear()
                            self.frame_key, self.frame_data = None, bytearray()
                            for intervals in self.intervals.values():
                                intervals.clear()
                        position = max(0, st.st_size-4*1024*1024)
                        f.seek(position)
                        if position:
                            f.readline()
                        position = f.tell()
                        pending = b''
                        identity = ident
                    f.seek(position)
                    chunk = f.read(1024*1024)
                    position = f.tell()
                pending += chunk
                lines = pending.split(b'\n')
                pending = lines.pop()
                if len(pending) > 2*1024*1024:
                    pending = b''
                with self.lock:
                    for line in lines:
                        try:
                            self.accept(json.loads(line.decode('latin1')))
                        except (ValueError, TypeError, KeyError):
                            continue
                    self.error = None
            except OSError:
                with self.lock:
                    self.error = '수신 파일 접근 불가'
            time.sleep(.2)
