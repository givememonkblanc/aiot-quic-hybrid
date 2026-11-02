import copy
import unittest
from fixed_metrics import summarize


class FixedMetricsTests(unittest.TestCase):
    def setUp(self):
        self.config=dict(count=4,rate=2,qos=0,deadline_ms=200,pause_at_ms=500,pause_ms=500)
        self.raw=dict(generated=[dict(seq=i,due_us=i*500000,generated_us=i*500000+100,decision='admitted') for i in range(4)],
            received=[dict(seq=0,received_us=100000,latency_us=100000,qos=0),
                      dict(seq=1,received_us=1100000,latency_us=600000,qos=0),
                      dict(seq=2,received_us=1150000,latency_us=150000,qos=0)],
            submitted=[dict(seq=i,ok=True) for i in range(4)],invalid_payloads=0,
            application_queue_peak_bytes=128,application_pending_bytes_at_cutoff=32,worker_pending_at_cutoff=True)

    def test_deadline_denominator_includes_missing_and_late(self):
        r=summarize(self.raw,self.config)
        self.assertEqual(r['delivered_fraction'],.75)
        self.assertEqual(r['deadline_fraction'],.5)
        self.assertEqual(r['resume_to_first_receive_ms'],100)
        self.assertEqual(r['resume_to_first_new_generation_receive_ms'],150)
        self.assertEqual(r['receive_gap_across_application_pause_ms'],1000)

    def test_duplicates_use_first_received(self):
        row=copy.deepcopy(self.raw['received'][0]);row.update(received_us=300000,latency_us=300000)
        self.raw['received'].insert(0,row)
        r=summarize(self.raw,self.config)
        self.assertEqual(r['duplicates'],1)
        self.assertEqual(r['deadline_fraction'],.5)

    def test_incomplete_input_rejected(self):
        self.raw['generated'].pop()
        with self.assertRaises(ValueError):summarize(self.raw,self.config)

    def test_clock_corruption_rejected(self):
        self.raw['received'][0]['latency_us']=100
        with self.assertRaises(ValueError):summarize(self.raw,self.config)

    def test_schedule_drift_rejected(self):
        self.raw['generated'][1]['due_us']+=100
        with self.assertRaises(ValueError):summarize(self.raw,self.config)

    def test_qos_downgrade_rejected(self):
        self.config['qos']=1
        with self.assertRaises(ValueError):summarize(self.raw,self.config)

    def test_no_delivery_is_not_success(self):
        self.raw['received']=[]
        r=summarize(self.raw,self.config)
        self.assertEqual(r['deadline_fraction'],0)
        self.assertIsNone(r['received_p95_ms'])
        self.assertIsNone(r['resume_to_first_receive_ms'])


if __name__=='__main__':unittest.main()
