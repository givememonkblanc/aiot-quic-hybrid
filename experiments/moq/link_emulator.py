"""UDP link between a host publisher and a receiver, with the impairments of flow_shaper.py.

Only the device-to-receiver direction is shaped, as in the device experiments: a rate cap,
added delay, a bounded queue, random loss and outages. The reverse direction is forwarded
unchanged. Host emulation of a link; it does not reproduce Wi-Fi behaviour."""

import asyncio
import random
import time


class Link:
    def __init__(self, target_port, rate_kbit=512, delay_ms=25, queue_packets=64, loss_percent=0.0,
                 outages=(), seed=1):
        self.target = ("127.0.0.1", target_port)
        self.rate = rate_kbit * 1000 / 8.0  # bytes per second
        self.delay = delay_ms / 1000.0
        self.queue_packets = queue_packets
        self.loss = loss_percent / 100.0
        self.outages = sorted(outages)  # (start, end) seconds after start()
        self.random = random.Random(seed)
        self.queue = []
        self.busy_until = 0.0
        self.epoch = None
        self.clients = {}  # device address -> upstream transport
        self.stats = dict(forwarded=0, bytes=0, dropped_queue=0, dropped_loss=0, dropped_outage=0)
        self.transport = None
        self.port = None

    async def start(self):
        loop = asyncio.get_running_loop()
        link = self

        class Front(asyncio.DatagramProtocol):
            def connection_made(self, transport):
                link.transport = transport

            def datagram_received(self, data, addr):
                link.from_device(data, addr)

        transport, _ = await loop.create_datagram_endpoint(Front, local_addr=("127.0.0.1", 0))
        self.port = transport.get_extra_info("sockname")[1]
        self.epoch = time.monotonic()
        return self.port

    def restart_clock(self):
        self.epoch = time.monotonic()

    def elapsed(self):
        return time.monotonic() - self.epoch

    def in_outage(self, at=None):
        at = self.elapsed() if at is None else at
        return any(start <= at < end for start, end in self.outages)

    def from_device(self, data, addr):
        if self.in_outage():
            self.stats["dropped_outage"] += 1
            return
        if self.random.random() < self.loss:
            self.stats["dropped_loss"] += 1
            return
        now = time.monotonic()
        waiting = sum(1 for due in self.queue if due > now)
        if waiting >= self.queue_packets:
            self.stats["dropped_queue"] += 1
            return
        # Serialisation at the rate cap, then the added delay.
        start = max(now, self.busy_until)
        self.busy_until = start + len(data) / self.rate
        due = self.busy_until + self.delay
        self.queue = [d for d in self.queue if d > now] + [self.busy_until]
        asyncio.get_running_loop().call_later(max(0.0, due - now), self.deliver, data, addr)

    def deliver(self, data, addr):
        # A packet that would arrive inside an outage is lost as well.
        if self.in_outage():
            self.stats["dropped_outage"] += 1
            return
        asyncio.ensure_future(self.upstream(data, addr))

    async def upstream(self, data, addr):
        up = self.clients.get(addr)
        if up is None:
            loop = asyncio.get_running_loop()
            link = self

            class Back(asyncio.DatagramProtocol):
                def datagram_received(self, reply, _):
                    link.transport.sendto(reply, addr)

            up, _ = await loop.create_datagram_endpoint(Back, remote_addr=self.target)
            self.clients[addr] = up
        up.sendto(data)
        self.stats["forwarded"] += 1
        self.stats["bytes"] += len(data)

    def close(self):
        if self.transport:
            self.transport.close()
        for up in self.clients.values():
            up.close()
