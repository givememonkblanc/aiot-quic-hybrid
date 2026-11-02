import unittest
from receiver import DualReceiver, Receiver, vi, read_vi, msg, read_message, setup_fields, NeedMore


class WireCases(unittest.TestCase):
    def test_spec_vectors(self):
        for raw, value in [
            ("25", 37),
            ("8025", 37),
            ("bbbd", 15293),
            ("ed7f3e7d", 226442877),
            ("ffffffffffffffffff", 2**64 - 1),
        ]:
            b = bytes.fromhex(raw)
            self.assertEqual(read_vi(b), (value, len(b)))
            for n in range(len(b)):
                with self.assertRaises(NeedMore):
                    read_vi(b[:n])

    def test_message_limits(self):
        with self.assertRaises(ValueError):
            read_message(b"\x03\xff\xff")
        wire = msg(0x2F00, b"\x01\x01/")
        for n in range(len(wire)):
            with self.assertRaises(NeedMore):
                read_message(wire[:n])
        self.assertEqual(read_message(wire), (0x2F00, b"\x01\x01/", len(wire)))

    def test_setup_validation(self):
        self.assertEqual(setup_fields(b"\x01\x01/\x04\x01x"), {1: b"/", 5: b"x"})
        with self.assertRaises(ValueError):
            setup_fields(b"\x01\x01/\x00\x01/")
        with self.assertRaises(ValueError):
            setup_fields(vi(2**64 - 1) + b"\x00" + vi(1) + b"\x00")
        with self.assertRaises(ValueError):
            setup_fields(b"\x01\x08bad")

    def test_done_waits_for_fin_and_objects(self):
        r = Receiver.__new__(Receiver)
        r.request = 1
        r.subscribed = True
        r.expected = None
        r.count = 0
        r.fins = set()
        r.log = lambda *a, **kw: None
        r.buffers = {1: bytearray(msg(0x0B, vi(2) + vi(3) + vi(0)))}
        r.consume(1)
        self.assertIsNone(r.expected)
        r.fins.add(1)
        r.consume(1)
        self.assertEqual(r.expected, 3)
        self.assertEqual(r.count, 0)  # late data streams must still be accepted


class DualCases(unittest.TestCase):
    def receiver(self):
        from unittest.mock import Mock

        r = DualReceiver.__new__(DualReceiver)
        r._quic = Mock()
        r.transmit = Mock()
        r.log = Mock()
        r.all_finished = False
        r.requests = {1: 1, 5: 2}
        r.fins = set()
        r.buffers = {}
        r.control = None
        r.stream_alias = {}
        r.stream_group = {}
        r.unknown_resets = set()
        r.tracks = {
            a: dict(
                name=str(a),
                subscribed=True,
                expected=None,
                groups=set(),
                resets=set(),
                request=s,
                finished=False,
            )
            for s, a in r.requests.items()
        }
        return r

    def test_independent_done(self):
        r = self.receiver()
        r.buffers[1] = bytearray(msg(0x0B, vi(2) + vi(1) + vi(0)))
        r.fins.add(1)
        r.consume(1)
        self.assertEqual(r.tracks[1]["expected"], 1)
        self.assertFalse(r.tracks[1]["finished"])
        r.tracks[1]["groups"].add(10)
        r.maybe_finish()
        self.assertTrue(r.tracks[1]["finished"])
        self.assertFalse(r.tracks[2]["finished"])
        self.assertFalse(r.all_finished)
        r._quic.send_stream_data.assert_called_once_with(1, b"", end_stream=True)
        r.maybe_finish()
        r._quic.send_stream_data.assert_called_once()

    def test_done_preserves_sender_diagnostic_reason(self):
        r = self.receiver()
        reason = b'{"schema":1,"completed":0,"ack_on_time":0}'
        r.buffers[1] = bytearray(msg(0x0B, vi(2) + vi(0) + vi(len(reason)) + reason))
        r.fins.add(1)
        r.consume(1)
        r.log.assert_any_call("publish_done", alias=1, streams=0, reason=reason.decode())

    def test_alias_response_mismatch(self):
        r = self.receiver()
        r.tracks[2]["subscribed"] = False
        r.buffers[5] = bytearray(msg(4, b"\x01\x00"))
        with self.assertRaises(ValueError):
            r.consume(5)

    def test_identified_timeout_reset_counts_for_publish_done(self):
        r = self.receiver()
        r.tracks[1]["expected"] = 1
        r.buffers[6] = bytearray(b"\x78" + vi(1) + vi(9))
        r.on_stream_reset(6, 2)
        self.assertTrue(r.tracks[1]["finished"])
        self.assertEqual(r.tracks[1]["resets"], {6})
        r._quic.send_stream_data.assert_called_once_with(1, b"", end_stream=True)

    def test_preheader_reset_remains_unattributed(self):
        r = self.receiver()
        r.buffers[6] = bytearray()
        r.on_stream_reset(6, 2)
        self.assertEqual(r.unknown_resets, {6})
        self.assertFalse(r.tracks[1]["finished"])

    def test_invalid_ai_identity(self):
        import json

        r = self.receiver()
        data = json.dumps(dict(schema=1, seq=8, backend=3, result_hex="d102" + "00" * 18)).encode()
        r.buffers[6] = bytearray(b"\x78" + vi(2) + vi(9) + vi(0) + vi(len(data)) + data)
        r.fins.add(6)
        with self.assertRaises(ValueError):
            r.consume(6)


if __name__ == "__main__":
    unittest.main()
