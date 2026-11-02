import json
import tempfile
import unittest
from pathlib import Path
from analyze_remote import summarize


class SummaryTests(unittest.TestCase):
    def events(self):
        return [
            dict(t=0, event="handshake", alpn="moqt-21"),
            dict(t=0.1, event="subscribed", alias=1),
            dict(t=0.2, event="subscribed", alias=2),
            *[
                dict(
                    t=i,
                    event="object",
                    alias=2,
                    group=i,
                    bytes=100,
                    result=dict(
                        observed_us=2000000,
                        capture_ms=1000,
                        detections=1,
                        prev_ack_valid=True,
                        prev_ack_group=7,
                        prev_source_to_ack_us=3000000,
                    ),
                )
                for i in (1, 2)
            ],
            dict(t=3, event="track_finished", alias=1, objects=0, resets=0),
            dict(t=3, event="track_finished", alias=2, objects=2, resets=0),
            dict(t=3, event="finished"),
        ]

    def summarize(self, rows):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / "events.jsonl").write_text("".join(json.dumps(x) + "\n" for x in rows))
            return summarize(p)

    def test_ack_piggyback_is_not_double_counted(self):
        out = self.summarize(self.events())
        self.assertEqual(out["ai"]["source_to_ack_samples"], 1)
        self.assertEqual(out["ai"]["source_to_ack_p95_s"], 3)
        self.assertEqual(out["ai"]["capture_to_observed_p95_s"], 1)

    def test_conflicting_ack_telemetry_rejected(self):
        rows = self.events()
        rows[4]["result"]["prev_source_to_ack_us"] = 4000000
        with self.assertRaises(ValueError):
            self.summarize(rows)

    def test_sender_receiver_mismatch_rejected(self):
        rows = self.events()
        rows.insert(
            -1,
            dict(
                t=3,
                event="publish_done",
                alias=2,
                streams=2,
                reason=json.dumps(dict(schema=1, completed=1, timeout_resets=0, ack_on_time=1)),
            ),
        )
        with self.assertRaises(ValueError):
            self.summarize(rows)

    def test_receiver_fin_can_precede_sender_reset(self):
        rows = self.events()
        rows.insert(
            -1,
            dict(
                t=3,
                event="publish_done",
                alias=2,
                streams=2,
                reason=json.dumps(dict(schema=1, completed=1, timeout_resets=1, ack_on_time=1)),
            ),
        )
        out = self.summarize(rows)["sender_totals"]["2"]
        self.assertEqual(out["receiver_completed"], 2)
        self.assertEqual(out["receiver_complete_sender_unconfirmed"], 1)
        self.assertEqual(out["ack_on_time"], 1)

    def test_unknown_reset_invalidates_trial(self):
        rows = self.events()
        rows.insert(-1, dict(t=2, event="reset", alias=None, stream=6, code=2))
        with self.assertRaises(ValueError):
            self.summarize(rows)

    def test_valid_sender_accounting(self):
        rows = self.events()
        rows.insert(
            -1,
            dict(
                t=3,
                event="publish_done",
                alias=2,
                streams=2,
                reason=json.dumps(dict(schema=1, completed=2, timeout_resets=0, ack_on_time=2)),
            ),
        )
        self.assertEqual(self.summarize(rows)["sender_totals"]["2"]["ack_on_time"], 2)


if __name__ == "__main__":
    unittest.main()
