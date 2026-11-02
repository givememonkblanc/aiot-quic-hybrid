#!/usr/bin/env python3
"""Compares the MOQT configurations with MQTT under one packet-level link emulation.

Every cell runs one model device in a network namespace (netns_link.py) against a receiver
on the host: the MOQT experiment receiver, or a mosquitto broker with a subscriber. The
workload, the result queue, the link and the outage trace are the same for all protocols.
Outage traces are drawn per seed: exponential times between outages and outage lengths.

  suite  runs all cells, several at a time
  cell   runs one cell (used by suite)

Host model: the numbers describe the protocols and rules, not the ESP32."""

import argparse
import asyncio
import json
import random
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

BASE = Path(__file__).resolve().parent
NAMES = dict(A="MOQT A: draft defaults", B="MOQT B: delivery timeout", C="MOQT C: results first",
             D="MOQT D: mission profile", M1="MQTT QoS 1, persistent session", M0="MQTT QoS 0")
TOTAL, NEW = "14336", "14338"


def trace(seed, produce_s, mean_up_s, mean_outage_s, max_outage_s):
    """Outages as (start, end) seconds after the start of the workload."""
    rng, t, out = random.Random(seed * 7919 + 17), 0.0, []
    while True:
        t += max(3.0, rng.expovariate(1.0 / mean_up_s))
        length = min(max_outage_s, max(1.0, rng.expovariate(1.0 / mean_outage_s)))
        if t + length >= produce_s - 2.0:
            return out
        out.append((round(t, 2), round(t + length, 2)))
        t += length


def metrics(ledger, events, outages, start):
    produced = {r["group"]: r for r in ledger}
    arrivals = {}
    for e in events:
        if e.get("event") == "object" and e.get("alias") == 2 and e["group"] in produced and e["group"] not in arrivals:
            arrivals[e["group"]] = e
    truth = [(r["capture_t"], r["passages_total"]) for r in ledger]
    known, cumulative, summed = [], 0, 0
    for e in sorted(arrivals.values(), key=lambda e: e["t"]):
        cumulative = max(cumulative, produced[e["group"]]["passages_total"])
        summed += produced[e["group"]]["passages_new"]
        known.append((e["t"], cumulative, summed))
    recovery = []
    for _, end in outages:
        at = start + end
        for t, c, _ in known:
            if t >= at and c == max([v for u, v in truth if u <= t], default=0):
                recovery.append(t - at)
                break
    lags = [arrivals[g]["t"] - produced[g]["capture_t"] for g in arrivals]
    quiet = [arrivals[g]["t"] - produced[g]["capture_t"] for g in arrivals
             if not any(start + a - 0.5 <= produced[g]["capture_t"] <= start + b + 5.0 for a, b in outages)]
    device_count = truth[-1][1] if truth else 0
    images = [e for e in events if e.get("event") == "object" and e.get("alias") == 3]
    pct = lambda v, q: sorted(v)[min(len(v) - 1, round(q * (len(v) - 1)))] if v else None  # noqa: E731
    return dict(
        results_produced=len(produced), results_received=len(arrivals),
        results_recovered=sum(1 for e in arrivals.values() if e.get("recovered")),
        events_produced=device_count, events_received=sum(produced[g]["passages_new"] for g in arrivals),
        error_cumulative=device_count - (known[-1][1] if known else 0),
        error_summed=device_count - (known[-1][2] if known else 0),
        recovery_s=recovery, outages=len(outages),
        lag_p50=pct(lags, .5), lag_p95=pct(lags, .95), lag_quiet_p50=pct(quiet, .5), lag_quiet_p95=pct(quiet, .95),
        images_received=len(images))


async def moqt_receiver(host, out, cert, key, idle, recover, stop):
    from aioquic.asyncio import serve
    from aioquic.quic.configuration import QuicConfiguration

    from mission_compare import _quiet, _record
    from mission_device import ALPN
    from receiver_draft21 import ExperimentReceiver

    config = QuicConfiguration(is_client=False, alpn_protocols=[ALPN], max_datagram_frame_size=1200,
                               max_data=2**22, max_stream_data=2**20, idle_timeout=idle)
    config.load_cert_chain(cert, key)
    shared = {}

    def make(*a, **kw):
        r = ExperimentReceiver(*a, out=out, authority="localhost:14568", evidence="push", image_track=b"evidence/jpeg",
                               status=False, fetch_retry_s=0.1, fetch_window_s=2.0, recover=recover,
                               recover_grace_s=0.3, shared=shared, **kw)
        r.write = lambda record, r=r: _quiet(r, record)
        r.record = lambda event, r=r, **kw: _record(r, event, **kw)
        return r

    server = await serve(host, 0, configuration=config, create_protocol=make)
    port = server._transport.get_extra_info("sockname")[1]
    return server, port, shared


class MqttReceiver:
    """A mosquitto broker on the host side of the link and a subscriber next to it."""

    def __init__(self, host, port, out):
        import paho.mqtt.client as mqtt

        self.out, self.port = out, port
        conf = out / "mosquitto.conf"
        conf.write_text(f"listener {port} {host}\nlistener {port + 1000} 127.0.0.1\nallow_anonymous true\n"
                        "persistence false\nmax_queued_messages 1000\nlog_type none\n")
        self.broker = subprocess.Popen(["mosquitto", "-c", str(conf)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        self.lock = threading.Lock()
        c = self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="receiver", protocol=mqtt.MQTTv5)
        c.on_message = self.on_message
        c.on_connect = lambda client, *a: client.subscribe([("aiot/esp32/ai/events", 1), ("aiot/esp32/evidence/jpeg", 0)])
        c.connect("127.0.0.1", port + 1000, keepalive=30)
        c.loop_start()

    def on_message(self, client, userdata, message):
        now = time.monotonic()
        if message.topic.endswith("ai/events"):
            body = json.loads(message.payload)
            record = dict(t=now, event="object", alias=2, group=body["seq"], bytes=len(message.payload),
                          duplicate=bool(message.dup), recovered=False,
                          properties={TOTAL: body["passages_total"], NEW: body["passages_new"]})
        else:
            head = json.loads(message.payload[1:1 + message.payload[0]])
            record = dict(t=now, event="object", alias=3, group=head["seq"], bytes=len(message.payload), recovered=False)
        with self.lock, (self.out / "events.jsonl").open("a") as f:
            f.write(json.dumps(record) + "\n")

    def close(self):
        self.client.loop_stop()
        self.client.disconnect()
        self.broker.terminate()
        self.broker.wait()


async def cell(a):
    from mission_compare import certificate
    from mission_device import POLICIES
    from netns_link import NetnsLink

    a.out.mkdir(parents=True, exist_ok=True)
    outages = trace(a.seed, a.produce, a.mean_up, a.mean_outage, a.max_outage) if a.outages is None else \
        [tuple(float(x) for x in o.split(":")) for o in a.outages if o]
    link = NetnsLink(a.index, rate_kbit=a.rate_kbit, delay_ms=a.delay_ms)
    link.up()
    receiver = server = None
    try:
        with tempfile.TemporaryDirectory(prefix="protocol-cert-") as d:
            cert, key = certificate(Path(d))
            Path(d).chmod(0o755)
            cert.chmod(0o644)
            if a.policy in ("M0", "M1"):
                receiver = MqttReceiver(link.host_ip, 18000 + a.index, a.out)
                port = receiver.port
            else:
                server, port, _ = await moqt_receiver(link.host_ip, a.out, cert, key, a.idle_timeout,
                                                      POLICIES[a.policy]["recover"], None)
            start = time.monotonic() + 2.5
            argv = [sys.executable, str(BASE / "device_main.py"), "--policy", a.policy, "--host", link.host_ip,
                    "--port", str(port), "--cert", str(cert), "--out", str(a.out), "--seed", str(a.seed),
                    "--produce", str(a.produce), "--drain", str(a.drain), "--inference", str(a.inference),
                    "--deadline", str(a.deadline), "--queue-results", str(a.queue_results),
                    "--idle-timeout", str(a.idle_timeout), "--retry", str(a.retry), "--attempt", str(a.attempt),
                    "--events", a.events, "--start-at", repr(start)]
            log = (a.out / "device.log").open("w")
            device = subprocess.Popen(link.command(argv), cwd=BASE, stdout=log, stderr=subprocess.STDOUT)
            edges = sorted([(s, True) for s, _ in outages] + [(e, False) for _, e in outages])
            for at, blocked in edges:
                await asyncio.sleep(max(0.0, start + at - time.monotonic()))
                if device.poll() is not None:
                    break
                link.set_blocked(blocked)
                link.actions.append(dict(target_s=at, actual_s=time.monotonic() - start, blocked=blocked))
            deadline = start + a.produce + a.drain + 20
            while device.poll() is None and time.monotonic() < deadline:
                await asyncio.sleep(0.2)
            if device.poll() is None:
                device.kill()
            log.close()
            await asyncio.sleep(0.5)
    finally:
        if link.blocked:
            link.set_blocked(False)
        if server is not None:
            server.close()
        if receiver is not None:
            receiver.close()
        link.down()
    ledger = [json.loads(x) for x in (a.out / "ledger.jsonl").read_text().splitlines()]
    events_file = a.out / "events.jsonl"
    events = [json.loads(x) for x in events_file.read_text().splitlines()] if events_file.exists() else []
    info = json.loads((a.out / "device.json").read_text())
    report = metrics(ledger, events, outages, info["start"])
    report.update(policy=a.policy, name=NAMES[a.policy], seed=a.seed, queue_results=a.queue_results,
                  outage_trace=outages, outage_actions=link.actions, device=info["stats"],
                  link=dict(rate_kbit=a.rate_kbit, delay_ms=a.delay_ms),
                  errors=[e.get("reason") for e in events if e.get("event") == "error"])
    (a.out / "report.json").write_text(json.dumps(report, indent=1) + "\n")
    return report


def suite(a):
    cells = [(q, p, s) for q in a.queue for p in a.policy for s in range(a.first_seed, a.first_seed + a.seeds)]
    running, reports, index = {}, [], 0
    free = list(range(a.jobs))
    pending = list(cells)
    while pending or running:
        while pending and free:
            q, p, s = pending.pop(0)
            slot = free.pop(0)
            out = a.out / f"q{q}-{p}-s{s}"
            argv = [sys.executable, __file__, "cell", "--index", str(a.first_index + slot), "--policy", p, "--seed", str(s),
                    "--queue-results", str(q), "--out", str(out)]
            for k in ("produce", "drain", "inference", "deadline", "idle_timeout", "retry", "attempt", "events",
                      "rate_kbit", "delay_ms", "mean_up", "mean_outage", "max_outage"):
                argv += ["--" + k.replace("_", "-"), str(getattr(a, k))]
            running[slot] = (subprocess.Popen(argv, cwd=BASE, stdout=(a.out / f"cell-{slot}.log").open("a"),
                                              stderr=subprocess.STDOUT), out)
            index += 1
        time.sleep(1.0)
        for slot, (proc, out) in list(running.items()):
            if proc.poll() is None:
                continue
            del running[slot]
            free.append(slot)
            if (out / "report.json").exists():
                reports.append(json.loads((out / "report.json").read_text()))
                r = reports[-1]
                print(f"{out.name}: {r['results_received']}/{r['results_produced']} results, "
                      f"count error {r['error_cumulative']} (summed {r['error_summed']})", flush=True)
            else:
                print(f"{out.name}: failed, see {a.out}/cell-{slot}.log", flush=True)
    (a.out / "summary.json").write_text(json.dumps(dict(
        schema=1, kind="host model, network namespace link", options={k: v for k, v in vars(a).items() if k not in ("out", "func")},
        reports=reports), indent=1, default=str) + "\n")


def options(p):
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--produce", type=float, default=300.0)
    p.add_argument("--drain", type=float, default=60.0)
    p.add_argument("--inference", type=float, default=1.0)
    p.add_argument("--deadline", type=float, default=10.0)
    p.add_argument("--idle-timeout", type=float, default=3.0)
    p.add_argument("--retry", type=float, default=1.0)
    p.add_argument("--attempt", type=float, default=2.0)
    p.add_argument("--events", default="0,0,0,1")
    p.add_argument("--rate-kbit", type=int, default=160)
    p.add_argument("--delay-ms", type=int, default=25)
    p.add_argument("--mean-up", type=float, default=40.0, help="mean time between outages, seconds")
    p.add_argument("--mean-outage", type=float, default=12.0)
    p.add_argument("--max-outage", type=float, default=45.0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    c = sub.add_parser("cell")
    options(c)
    c.add_argument("--index", type=int, default=0)
    c.add_argument("--policy", required=True, choices=list(NAMES))
    c.add_argument("--seed", type=int, default=1)
    c.add_argument("--queue-results", type=int, default=32)
    c.add_argument("--outages", nargs="*", help="START:END seconds; default: the trace of the seed")
    s = sub.add_parser("suite")
    options(s)
    s.add_argument("--policy", nargs="+", default=list(NAMES), choices=list(NAMES))
    s.add_argument("--queue", nargs="+", type=int, default=[32])
    s.add_argument("--seeds", type=int, default=10)
    s.add_argument("--first-seed", type=int, default=1)
    s.add_argument("--jobs", type=int, default=6)
    s.add_argument("--first-index", type=int, default=0)
    a = p.parse_args()
    if a.command == "cell":
        report = asyncio.run(cell(a))
        print(json.dumps({k: report[k] for k in ("policy", "results_received", "results_produced", "error_cumulative",
                                                 "error_summed", "recovery_s", "lag_quiet_p95")}))
    else:
        a.out.mkdir(parents=True, exist_ok=True)
        suite(a)


if __name__ == "__main__":
    main()
