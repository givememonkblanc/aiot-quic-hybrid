import json
import unittest
from collect import decode

class DecodeTests(unittest.TestCase):
    def test_measurement_and_overlapping_dma(self):
        payload = dict(kind='ram', schema=1, internal_free=25, internal_total=100,
                       psram_free=600, psram_total=1000, dma_free=20,
                       queue_bytes=1234, queue_budget=65536, target_fps=30)
        row = dict(kind='publish', topic='aiot/diagnostics/ram', cid='test', payload=json.dumps(payload))
        result = decode(json.dumps(row))
        self.assertEqual(result['internal_used_pct'], 75)
        self.assertEqual(result['psram_used_pct'], 40)
        self.assertEqual(result['dma_free'], 20)
        self.assertEqual(result['queue_bytes'], 1234)
        self.assertEqual(result['target_fps'], 30)
        payload['internal_free'] = 101
        row['payload'] = json.dumps(payload)
        self.assertIsNone(decode(json.dumps(row)))

    def test_ignore_other_tracks_and_partial_rows(self):
        self.assertIsNone(decode('{'))
        self.assertIsNone(decode(json.dumps(dict(kind='datagram', track=3, payload='image'))))

if __name__ == '__main__':
    unittest.main()
