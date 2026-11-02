import json
import struct
import unittest
from research_status import ReceiverStatus


def packet(track, seq, t, gen=1000, raw=b'', cid='a'):
    return dict(kind='datagram', track=track, seq=seq, t=int(t*1e6),
                gen_ts_ms=gen, cid=cid, payload=raw.decode('latin1'))


def ai(t, gen=1000, cid='a'):
    raw = bytes([0xd1, 2, 2, 0]) + struct.pack('<HHHHI', 640, 640, 96, 96, 8) + bytes([127, 0, 0, 0])
    return packet(2, 8, t, gen, raw, cid)


class StatusTests(unittest.TestCase):
    def test_frame_reassembly_and_matching_both_orders(self):
        for ai_time in (9.9, 10.2):
            s = ReceiverStatus('/unused')
            s.accept(ai(ai_time))
            s.accept(packet(3, 1, 9.8, raw=b'\xff\xd8abc'))
            self.assertEqual(s.snapshot(10)['rows']['image']['state'], 'waiting')
            s.accept(packet(3, 2, 10, raw=b'def\xff\xd9'))
            result = s.snapshot(11)
            self.assertEqual(result['alignment']['same_frame_receive_delta_ms'], round((ai_time-10)*1000, 1))
            self.assertEqual(result['rows']['ai']['data']['raw_score'], 127)
            self.assertEqual(result['rows']['image']['age_s'], 1)

    def test_missing_chunk_not_counted(self):
        s = ReceiverStatus('/unused')
        s.accept(packet(3, 1, 10, raw=b'\xff\xd8abc'))
        s.accept(packet(3, 3, 11, raw=b'\xff\xd9'))
        self.assertEqual(s.snapshot(12)['rows']['image']['state'], 'waiting')

    def test_activity_on_other_track_does_not_refresh_ai(self):
        s = ReceiverStatus('/unused')
        s.accept(ai(1));s.accept(ai(2))
        s.accept(packet(3, 1, 10, raw=b'\xff\xd8\xff\xd9'))
        r = s.snapshot(10)['rows']
        self.assertEqual(r['ai']['age_s'], 8)
        self.assertEqual(r['ai']['state'], 'late')
        self.assertEqual(r['image']['state'], 'recent')

    def test_event_silence_is_not_delay(self):
        s = ReceiverStatus('/unused')
        s.accept(dict(kind='publish',t=1000000,cid='a',payload=json.dumps(dict(seq=1,critical=True,gen_us=1))))
        self.assertEqual(s.snapshot(100)['rows']['critical']['state'], 'event')

    def test_session_mismatch_does_not_join(self):
        s = ReceiverStatus('/unused')
        s.accept(ai(1))
        s.accept(packet(3, 1, 2, raw=b'\xff\xd8\xff\xd9',cid='b'))
        self.assertIsNone(s.snapshot(3)['alignment']['same_frame_receive_delta_ms'])
        self.assertIsNone(s.snapshot(3)['alignment']['source_gap_ms'])


if __name__ == '__main__':
    unittest.main()
