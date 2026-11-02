import unittest
from analyze_evidence import coverage


class EvidenceTests(unittest.TestCase):
    def events(self):
        return [
            dict(
                event="object",
                alias=2,
                group=7,
                result=dict(seq=7, evidence_required=1, evidence_group=7),
            ),
            dict(event="object", alias=1, group=7),
        ]

    def stats(self):
        return {"1": dict(completed=1, ack_on_time=1), "2": dict(completed=1, ack_on_time=1)}

    def test_exact_source_pair(self):
        r = coverage(self.events(), self.stats())
        self.assertEqual(r["linked_complete_pairs"], 1)
        self.assertEqual(r["pair_ack_on_time_lower_bound"], 1)

    def test_different_preview_is_not_evidence(self):
        e = self.events()
        e[1]["group"] = 8
        r = coverage(e, self.stats())
        self.assertEqual(r["missing_evidence"], 1)
        self.assertEqual(r["orphan_images"], [8])

    def test_wrong_reference_rejected(self):
        e = self.events()
        e[0]["result"]["evidence_group"] = 8
        with self.assertRaises(ValueError):
            coverage(e, self.stats())

    def test_receiver_complete_sender_timeout_not_counted_on_time(self):
        s = self.stats()
        s["1"].update(completed=0, ack_on_time=0, timeout_resets=1)
        r = coverage(self.events(), s)
        self.assertEqual(r["linked_complete_pairs"], 1)
        self.assertEqual(r["pair_ack_on_time_lower_bound"], 0)

    def test_late_ack_does_not_imply_late_receive(self):
        s = self.stats()
        s["1"]["ack_on_time"] = 0
        r = coverage(self.events(), s)
        self.assertEqual(r["linked_complete_pairs"], 1)
        self.assertEqual(r["pair_ack_on_time_lower_bound"], 0)


if __name__ == "__main__":
    unittest.main()
