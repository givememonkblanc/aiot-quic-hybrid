import math
import unittest

from paper_metrics import delay_metrics
from pcap_metrics import summarize_packets
from energy_metrics import integrate


class PaperMetricTests(unittest.TestCase):
    def test_meter_units_and_irregular_sampling(self):
        result = integrate([(0, 5, .1), (1, 5, .3), (3, 5, .3)])
        self.assertEqual(result['energy_j'], 4)
        self.assertAlmostEqual(result['power_mean_w'], 4/3)

    def test_invalid_meter_readings_rejected(self):
        for samples in ([], [(0, 5, 1)], [(0, 5, 1), (0, 5, 1)],
                        [(0, 5, 1), (1, float('nan'), 1)]):
            with self.assertRaises(ValueError):
                integrate(samples)

    def test_delay_rsd_estimators_and_quantiles(self):
        values = delay_metrics([1, 2, 3, 4, 5])
        self.assertEqual(values['latency_p5_ms'], 1.2)
        self.assertEqual(values['latency_p25_ms'], 2)
        self.assertEqual(values['latency_p75_ms'], 4)
        self.assertAlmostEqual(values['latency_rsd_population'], math.sqrt(2)/3)
        self.assertAlmostEqual(values['latency_rsd_sample'], math.sqrt(2.5)/3)

    def test_single_message_has_no_sample_rsd(self):
        self.assertIsNone(delay_metrics([3])['latency_rsd_sample'])
        self.assertEqual(delay_metrics([3])['latency_rsd_population'], 0)

    def test_capture_counts_ack_and_retransmit_frames(self):
        rows = [('1', '10.0', '1200'), ('2', '10.1', '60'), ('3', '10.2', '1200')]
        result = summarize_packets(rows, 1, 3)
        self.assertEqual(result['wire_packets'], 3)
        self.assertEqual(result['wire_bytes'], 2460)
        self.assertAlmostEqual(result['packet_initial_to_first_publish_ms'], 200)

    def test_bad_capture_markers_fail_closed(self):
        rows = [('1', '1', '100'), ('2', '2', '100')]
        for initial, publish in [(1, None), (1, 3), (2, 1)]:
            with self.assertRaises(ValueError):
                summarize_packets(rows, initial, publish)
        with self.assertRaises(ValueError):
            summarize_packets([])


if __name__ == '__main__':
    unittest.main()
