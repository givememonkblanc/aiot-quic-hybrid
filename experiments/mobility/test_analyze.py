import json
import tempfile
import unittest
from pathlib import Path
from analyze import Ranges, analyze


class MeasurementTests(unittest.TestCase):
    def test_reordering_duplicates_and_partial_overlap(self):
        r = Ranges()
        self.assertEqual(r.add(100, 100), (100, 0))
        self.assertEqual(r.add(150, 100), (50, 0))
        self.assertEqual(r.add(100, 100), (0, 0))
        self.assertEqual(r.add(0, 100), (100, 250))
        self.assertEqual(r.add(0, 250), (0, 0))
        self.assertEqual(r.add(250, 50), (50, 50))

    def fixture(self, directory, multipath=True):
        def packet(t, path, offset, length, fin=False):
            return [t, path, 'transport', 'packet_received', {'header': {'packet_size': length+30},
                'frames': [{'frame_type': 'stream', 'id': 0, 'offset': offset, 'length': length, 'fin': fin}]}]
        doc = {'traces': [{'vantage_point': {'type': 'client'},
            'configuration': {'time_units': 'us'}, 'common_fields': {'reference_time': '100000000'},
            'event_fields': ['relative_time', 'path_id', 'category', 'event', 'data'],
            'events': [packet(100000, 0, 0, 100), packet(200000, 1 if multipath else 0, 200, 100, True),
                       packet(300000, 0, 0, 100), packet(1100000, 0, 100, 100)]}]}
        qlog = Path(directory)/'trace.qlog'
        qlog.write_text(json.dumps(doc))
        manifest = {'clock': 'CLOCK_MONOTONIC', 'client_start_monotonic_s': 100.,
            'mode': 'multipath', 'scenario': 'outage', 'bytes': 300, 'client_returncode': 0,
            'client_log': 'Enable multipath: Success.', 'expected_events': 1,
            'events': [{'kind': 'impairment', 'name': 'down', 'started_monotonic_s': 100.49,
                        'completed_monotonic_s': 100.5}]}
        return qlog, manifest

    def test_in_order_gap_and_monotonic_event_alignment(self):
        with tempfile.TemporaryDirectory() as d:
            qlog, manifest = self.fixture(d)
            s, rows = analyze(qlog, manifest)
        self.assertTrue(s['valid'])
        self.assertEqual(s['unique_stream_bytes'], 300)
        self.assertEqual(s['data_path_count'], 2)
        self.assertAlmostEqual(s['max_in_order_progress_gap_ms'], 1000)
        self.assertAlmostEqual(s['events'][0]['next_in_order_progress_ms'], 600)
        self.assertEqual(sum(row['in_order_bytes'] for row in rows), 300)

    def test_flag_alone_does_not_prove_multipath(self):
        with tempfile.TemporaryDirectory() as d:
            qlog, manifest = self.fixture(d, multipath=False)
            s, _ = analyze(qlog, manifest)
        self.assertFalse(s['valid'])
        self.assertIn('multipath_not_confirmed_on_two_data_paths', s['invalid_reasons'])

    def test_clock_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            qlog, manifest = self.fixture(d)
            manifest['client_start_monotonic_s'] = 1700000000
            with self.assertRaises(ValueError):
                analyze(qlog, manifest)

    def test_truncated_transfer_or_missing_event_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            qlog, manifest = self.fixture(d)
            manifest['bytes'] = 400
            manifest['expected_events'] = 2
            s, _ = analyze(qlog, manifest)
        self.assertFalse(s['valid'])
        self.assertIn('incomplete_or_unexpected_transfer', s['invalid_reasons'])
        self.assertIn('scenario_not_fully_applied', s['invalid_reasons'])


if __name__ == '__main__':
    unittest.main()
