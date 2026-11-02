import unittest
from unittest.mock import Mock

from flow_shaper import FlowShaper, OutageSchedule, parse_outages


def shaper():
    s = FlowShaper(512, 25, 64, 1)
    s.run = Mock(return_value="")
    s.created = True
    return s


class Outages(unittest.TestCase):
    def test_block_keeps_rate_and_delay_and_restores_the_loss(self):
        s = shaper()
        s.block()
        blocked = s.run.call_args.args
        self.assertEqual(blocked[:6], ("tc", "qdisc", "change", "dev", "moqv6test", "root"))
        self.assertIn("100%", blocked)
        self.assertIn("512kbit", blocked)
        self.assertIn("25ms", blocked)
        self.assertTrue(s.blocked)
        s.unblock()
        self.assertIn("1%", s.run.call_args.args)
        self.assertFalse(s.blocked)

    def test_outage_needs_an_owned_interface(self):
        s = FlowShaper()
        with self.assertRaises(ValueError):
            s.block()

    def test_parse(self):
        self.assertEqual(parse_outages(["30:8", "10:2.5"]), [(10.0, 12.5), (30.0, 38.0)])
        self.assertEqual(parse_outages(None), [])
        for bad in (["5:0"], ["-1:3"], ["x:1"], ["10:5", "12:1"]):
            with self.assertRaises(ValueError):
                parse_outages(bad)
        # An outage as long as the idle timeout ends the session instead of interrupting it.
        with self.assertRaises(ValueError):
            parse_outages(["10:20"], idle_timeout_s=20)
        self.assertEqual(parse_outages(["10:19.5"], idle_timeout_s=20), [(10.0, 29.5)])

    def test_schedule(self):
        s = shaper()
        schedule = OutageSchedule(s, [(10, 12), (30, 38)])
        self.assertEqual(schedule.next_in(4), 6)
        self.assertEqual(schedule.step(9.9), [])
        schedule.step(10.02)
        self.assertTrue(s.blocked)
        self.assertAlmostEqual(schedule.next_in(10.02), 1.98)
        schedule.step(40)  # a late poll applies every edge that is due, in order
        self.assertFalse(s.blocked)
        self.assertEqual([a["action"] for a in schedule.applied], ["block", "unblock", "block", "unblock"])
        self.assertIsNone(schedule.next_in(40))

    def test_cleanup_never_leaves_the_flow_blocked(self):
        s = shaper()
        schedule = OutageSchedule(s, [(10, 12)])
        schedule.step(10)
        schedule.finish()
        self.assertFalse(s.blocked)
        self.assertEqual(schedule.applied[-1]["reason"], "cleanup")
        schedule.finish()
        self.assertEqual(len(schedule.applied), 2)


if __name__ == "__main__":
    unittest.main()
