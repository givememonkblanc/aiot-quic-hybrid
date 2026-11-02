import asyncio
import time
import unittest

from evaluate_passages import evaluate
from link_emulator import Link


def ledger():
    return [
        dict(group=26, capture_t=1.0, passages_new=1, passages_total=1),
        dict(group=52, capture_t=1.4, passages_new=0, passages_total=1),
        dict(group=78, capture_t=1.8, passages_new=2, passages_total=3),
        dict(group=104, capture_t=2.2, passages_new=1, passages_total=4),
    ]


def result(group, t, **kw):
    return dict(event="object", alias=2, group=group, t=t, bytes=100, **kw)


class PassageAccounting(unittest.TestCase):
    def test_all_delivered(self):
        events = [result(26, 1.2), result(52, 1.5), result(78, 2.0), result(104, 2.5)]
        r = evaluate(ledger(), events)
        self.assertEqual((r["results_received"], r["passage_events_received"], r["count_error"]), (4, 4, 0))
        self.assertEqual(r["passage_event_delivery_ratio"], 1.0)
        self.assertAlmostEqual(r["result_lag_s"]["max"], 0.3)

    def test_a_missing_result_loses_its_events_but_not_the_total(self):
        # The cumulative total arrives with a later result; the two events of Group 78 do not.
        events = [result(26, 1.2), result(52, 1.5), result(104, 2.5)]
        r = evaluate(ledger(), events)
        self.assertEqual(r["results_missing"], [78])
        self.assertEqual((r["passage_events_missed"], r["count_error"]), (2, 0))

    def test_a_missing_last_result_shows_in_the_count(self):
        r = evaluate(ledger(), [result(26, 1.2), result(52, 1.5), result(78, 2.0)])
        self.assertEqual((r["true_total"], r["reported_total"], r["count_error"]), (4, 3, 1))

    def test_duplicates_and_recovery(self):
        events = [result(26, 1.2), result(26, 9.0), result(78, 4.0, recovered=True)]
        r = evaluate(ledger(), events, outages=[(1.3, 2.0)])
        self.assertEqual((r["results_received"], r["results_recovered"]), (2, 1))
        self.assertEqual((r["produced_during_outage"], r["received_of_outage"]), (2, 1))
        self.assertAlmostEqual(r["result_lag_s"]["max"], 2.2)  # the first copy counts

    def test_results_the_device_never_produced_are_an_error(self):
        with self.assertRaises(ValueError):
            evaluate(ledger(), [result(27, 1.0)])

    def test_images_are_counted_apart(self):
        events = [result(26, 1.2), dict(event="object", alias=3, group=26, t=1.9, bytes=12000),
                  dict(event="fetch_object", alias=3, group=78, t=3.0, bytes=11000)]
        r = evaluate(ledger(), events)
        self.assertEqual((r["images_received"], r["image_bytes_received"]), (2, 23000))


class LinkEmulation(unittest.IsolatedAsyncioTestCase):
    async def sink(self):
        loop = asyncio.get_running_loop()
        got = []

        class Sink(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                got.append((time.monotonic(), len(data)))
                self.transport.sendto(b"ack", addr)

            def connection_made(self, transport):
                self.transport = transport

        transport, _ = await loop.create_datagram_endpoint(Sink, local_addr=("127.0.0.1", 0))
        return transport, got

    async def source(self, port):
        loop = asyncio.get_running_loop()
        replies = []

        class Source(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                replies.append(data)

        transport, _ = await loop.create_datagram_endpoint(Source, remote_addr=("127.0.0.1", port))
        return transport, replies

    async def test_rate_delay_and_reverse_path(self):
        sink, got = await self.sink()
        link = Link(sink.get_extra_info("sockname")[1], rate_kbit=80, delay_ms=50)
        source, replies = await self.source(await link.start())
        start = time.monotonic()
        for _ in range(10):
            source.sendto(bytes(1000))  # 10 kB at 10 kB/s
        await asyncio.sleep(1.6)
        self.assertEqual(len(got), 10)
        self.assertGreater(got[0][0] - start, 0.12)  # 0.1 s of serialisation and 0.05 s of delay
        self.assertGreater(got[-1][0] - start, 0.95)
        self.assertLess(got[-1][0] - start, 1.4)
        self.assertEqual(len(replies), 10)  # the reverse direction is not shaped
        for t in (source, sink):
            t.close()
        link.close()

    async def test_outage_and_queue_limit(self):
        sink, got = await self.sink()
        link = Link(sink.get_extra_info("sockname")[1], rate_kbit=800, delay_ms=1, queue_packets=4,
                    outages=[(0.2, 0.5)])
        source, _ = await self.source(await link.start())
        link.restart_clock()
        source.sendto(b"before")
        await asyncio.sleep(0.3)
        source.sendto(b"during")
        await asyncio.sleep(0.3)
        source.sendto(b"after")
        await asyncio.sleep(0.1)
        self.assertEqual([n for _, n in got], [6, 5])
        self.assertEqual(link.stats["dropped_outage"], 1)
        for _ in range(12):
            source.sendto(bytes(1200))
        await asyncio.sleep(0.3)
        self.assertGreater(link.stats["dropped_queue"], 0)
        for t in (source, sink):
            t.close()
        link.close()


if __name__ == "__main__":
    unittest.main()
