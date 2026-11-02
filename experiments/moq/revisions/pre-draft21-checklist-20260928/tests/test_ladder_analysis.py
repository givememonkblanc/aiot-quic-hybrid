import unittest
from analyze_ladder import aggregate


class RunLevelTests(unittest.TestCase):
    def row(self, run, value):
        return dict(
            run=run,
            policy="low",
            network="128",
            preview_complete=value,
            preview_ack_on_time=value,
            preview_max_gap_offer_window_s=1,
            ai_complete=20,
            ai_source_to_ack_p95_s=3,
            min_free_internal_bytes=100000,
            min_free_psram_bytes=5000000,
        )

    def test_one_run_has_no_invented_confidence_interval(self):
        a = aggregate([self.row("r1", 100)])[0]
        self.assertEqual(a["n"], 1)
        self.assertIsNone(a["metrics"]["preview_complete"]["confidence_interval"])

    def test_frames_are_not_independent_runs(self):
        a = aggregate([self.row("r1", 100), self.row("r2", 200)])[0]
        self.assertEqual(a["n"], 2)
        self.assertEqual(a["metrics"]["preview_complete"]["mean"], 150)
        self.assertEqual(a["metrics"]["preview_complete"]["n"], 2)


if __name__ == "__main__":
    unittest.main()
