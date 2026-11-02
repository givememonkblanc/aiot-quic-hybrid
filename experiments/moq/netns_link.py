#!/usr/bin/env python3
"""Packet-level link between a device process and the host, for any protocol.

The device runs in its own network namespace behind a veth pair. netem shapes the
device-to-host direction (rate, delay, queue) and delays the reverse direction. An outage
drops every packet in both directions. The same link carries QUIC and TCP, so protocols
are compared under one emulation. Needs passwordless sudo for `ip` and `tc`."""

import getpass
import subprocess


class NetnsLink:
    def __init__(self, index, rate_kbit=160, delay_ms=25, queue_packets=64):
        if not 0 <= index < 200:
            raise ValueError("index out of range")
        self.ns = f"moqlink{index}"
        self.host_if, self.dev_if = f"mqh{index}", f"mqd{index}"
        self.host_ip, self.dev_ip = f"10.77.{index}.1", f"10.77.{index}.2"
        self.rate_kbit, self.delay_ms, self.queue_packets = rate_kbit, delay_ms, queue_packets
        self.blocked = False
        self.actions = []

    @staticmethod
    def sudo(*args, check=True):
        return subprocess.run(["sudo", "-n", *args], check=check, text=True, capture_output=True)

    def inside(self, *args, check=True):
        return self.sudo("ip", "netns", "exec", self.ns, *args, check=check)

    def netem(self, loss):
        up = ["rate", f"{self.rate_kbit}kbit", "delay", f"{self.delay_ms}ms", "limit", str(self.queue_packets),
              "loss", f"{loss}%"]
        down = ["delay", f"{self.delay_ms}ms", "loss", f"{loss}%"]
        return up, down

    def up(self):
        self.down()
        self.sudo("ip", "netns", "add", self.ns)
        self.sudo("ip", "link", "add", self.host_if, "type", "veth", "peer", "name", self.dev_if, "netns", self.ns)
        self.sudo("ip", "addr", "add", f"{self.host_ip}/24", "dev", self.host_if)
        self.sudo("ip", "link", "set", self.host_if, "up")
        self.inside("ip", "addr", "add", f"{self.dev_ip}/24", "dev", self.dev_if)
        self.inside("ip", "link", "set", self.dev_if, "up")
        self.inside("ip", "link", "set", "lo", "up")
        up, down = self.netem(0)
        self.inside("tc", "qdisc", "add", "dev", self.dev_if, "root", "netem", *up)
        self.sudo("tc", "qdisc", "add", "dev", self.host_if, "root", "netem", *down)
        self.blocked = False

    def set_blocked(self, blocked):
        up, down = self.netem(100 if blocked else 0)
        self.inside("tc", "qdisc", "change", "dev", self.dev_if, "root", "netem", *up)
        self.sudo("tc", "qdisc", "change", "dev", self.host_if, "root", "netem", *down)
        self.blocked = blocked

    def command(self, argv):
        """argv that runs in the namespace of the device as the calling user."""
        return ["sudo", "-n", "ip", "netns", "exec", self.ns, "sudo", "-n", "-u", getpass.getuser(), *argv]

    def stats(self):
        return self.inside("tc", "-s", "qdisc", "show", "dev", self.dev_if, check=False).stdout

    def down(self):
        self.sudo("ip", "link", "del", self.host_if, check=False)
        self.sudo("ip", "netns", "del", self.ns, check=False)
