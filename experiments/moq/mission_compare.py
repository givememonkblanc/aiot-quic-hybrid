#!/usr/bin/env python3
"""Compares four delivery rules for the counting mission under link outages, on the host.

  A  draft default      no priority, no delivery timeout
  B  delivery timeouts  results first; results and images expire after the deadline
  C  results first      results precede images; nothing else
  D  mission profile    results first and never dropped, queued like C, retained for FETCH,
                        losses announced with PRIOR_GROUP_ID_GAP; images expire

Every alternative gets the same workload, link, queue and the same ability to reconnect.
The publisher runs the firmware's C core; the device behaviour around it is modelled in
mission_device.py. The numbers describe this model, not the ESP32."""

import argparse
import asyncio
import datetime
import json
import tempfile
from pathlib import Path

from aioquic.asyncio import serve
from aioquic.quic.configuration import QuicConfiguration
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

from evaluate_passages import evaluate
from link_emulator import Link
from mission_device import ALPN, POLICIES, Device
from receiver_draft21 import ExperimentReceiver

SCENARIOS = {
    "steady": dict(outages=[], note="no outage"),
    "short_outage": dict(outages=[(5.0, 7.5)], note="2.5 s outage; the session survives"),
    "long_outage": dict(outages=[(5.0, 11.0)], note="6 s outage; the 3 s idle timeout ends the session"),
    "two_outages": dict(outages=[(3.0, 5.5), (8.0, 13.0)], note="2.5 s and 5 s outages in one run"),
}
TRACKS = [
    dict(ns=(b"aiot", b"esp32"), name=b"preview/jpeg", priority=224, group_order=2),
    dict(ns=(b"aiot", b"esp32"), name=b"ai/events", priority=32),
    dict(ns=(b"aiot", b"esp32"), name=b"evidence/jpeg", priority=128),
    dict(ns=(b"aiot", b"esp32"), name=b"status", priority=240),
]


def certificate(directory):
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=1)).not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), False)
        .sign(key, hashes.SHA256())
    )
    cert_path, key_path = directory / "cert.pem", directory / "key.pem"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
    return cert_path, key_path


async def run_cell(policy, scenario, out, cert, key, *, rate_kbit, delay_ms, seed, quiet=True, **device_options):
    out.mkdir(parents=True, exist_ok=True)
    idle = device_options.get("idle_timeout", 3.0)
    config = QuicConfiguration(is_client=False, alpn_protocols=[ALPN], max_datagram_frame_size=1200,
                               max_data=2**22, max_stream_data=2**20, idle_timeout=idle)
    config.load_cert_chain(cert, key)
    shared = {}
    receivers = []

    def make(*a, **kw):
        r = ExperimentReceiver(*a, out=out, authority="localhost:14568", evidence="push",
                               image_track=b"evidence/jpeg", status=False, fetch_retry_s=0.1, fetch_window_s=2.0,
                               recover=POLICIES[policy]["recover"], recover_grace_s=0.3, shared=shared, **kw)
        if quiet:
            r.write = lambda record, r=r: _quiet(r, record)
            r.record = lambda event, r=r, **kw: _record(r, event, **kw)
        receivers.append(r)
        return r

    server = await serve("127.0.0.1", 0, configuration=config, create_protocol=make)
    port = server._transport.get_extra_info("sockname")[1]
    link = Link(port, rate_kbit=rate_kbit, delay_ms=delay_ms, outages=SCENARIOS[scenario]["outages"], seed=seed)
    front = await link.start()
    device = Device(policy, front, cert, seed=seed, **device_options)
    link.restart_clock()
    start = await device.run(TRACKS)
    await asyncio.sleep(0.5)
    server.close()
    link.close()
    events = [json.loads(x) for x in (out / "events.jsonl").read_text().splitlines()] if (out / "events.jsonl").exists() else []
    (out / "ledger.jsonl").write_text("".join(json.dumps(r) + "\n" for r in device.ledger))
    outages = [(start + a, start + b) for a, b in SCENARIOS[scenario]["outages"]]
    report = evaluate(device.ledger, events, outages)
    report.update(policy=policy, policy_name=POLICIES[policy]["name"], scenario=scenario, seed=seed,
                  device=device.stats, link=link.stats,
                  receiver=dict(sessions=shared.get("sessions", 0),
                                gaps_detected=sum(e["event"] == "gap_detected" for e in events),
                                recover_requests=sum(e["event"] == "recover_request" for e in events),
                                lost=sum(e["event"] == "lost" for e in events),
                                errors=[e.get("reason") for e in events if e["event"] == "error"]))
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def _write(r, record):
    with (r.out / "events.jsonl").open("a") as f:
        f.write(json.dumps(record, default=str) + "\n")


def _quiet(r, record):
    if record["event"] not in ("fetch_object", "object"):
        _write(r, record)


def _record(r, event, **kw):
    import time

    record = dict(t=time.monotonic(), event=event, **kw)
    r.events.append(record)
    _write(r, record)
    r.changed.set()


def table(reports):
    head = "| 조건 | 대안 | 결과 도착/생성 | 통과 사건 도착/생성 | 개수 오차 | 결과 지연 p95 (s) | 복구한 결과 | 이미지 도착 | 세션 수 |"
    rows = [head, "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for r in reports:
        lag = r["result_lag_s"]["p95"]
        rows.append(
            f"| {r['scenario']} | {r['policy']} {r['policy_name']} | {r['results_received']}/{r['results_produced']} | "
            f"{r['passage_events_received']}/{r['passage_events_produced']} | {r['count_error']} | "
            f"{'—' if lag is None else f'{lag:.2f}'} | {r['results_recovered']} | {r['images_received']} | "
            f"{r['device']['sessions']} |"
        )
    return "\n".join(rows)


async def main(args):
    with tempfile.TemporaryDirectory(prefix="mission-cert-") as d:
        cert, key = certificate(Path(d))
        reports = []
        for scenario in args.scenario:
            for policy in args.policy:
                for repeat in range(args.repeats):
                    out = args.out / f"{scenario}-{policy}-r{repeat + 1}"
                    report = await run_cell(policy, scenario, out, cert, key, rate_kbit=args.rate_kbit,
                                            delay_ms=args.delay_ms, seed=args.seed + repeat,
                                            produce_s=args.produce, inference_s=args.inference, queue_results=args.queue_results,
                                            deadline_s=args.deadline, image_bytes=args.image_bytes)
                    report["repeat"] = repeat + 1
                    reports.append(report)
                    print(f"{scenario} {policy} r{repeat + 1}: results {report['results_received']}/"
                          f"{report['results_produced']}, events {report['passage_events_received']}/"
                          f"{report['passage_events_produced']}", flush=True)
    summary = dict(schema=1, kind="host model", rate_kbit=args.rate_kbit, delay_ms=args.delay_ms,
                   scenarios={k: SCENARIOS[k] for k in args.scenario}, queue_results=args.queue_results, reports=reports)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    (args.out / "summary.md").write_text(table(reports) + "\n")
    print(table(reports))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--policy", nargs="+", default=list(POLICIES), choices=list(POLICIES))
    p.add_argument("--scenario", nargs="+", default=list(SCENARIOS), choices=list(SCENARIOS))
    p.add_argument("--repeats", type=int, default=1)
    p.add_argument("--seed", type=int, default=20260928)
    p.add_argument("--rate-kbit", type=int, default=160)
    p.add_argument("--delay-ms", type=int, default=25)
    p.add_argument("--produce", type=float, default=14.0, help="seconds of result production")
    p.add_argument("--inference", type=float, default=0.4, help="seconds between results")
    p.add_argument("--deadline", type=float, default=3.0, help="deadline after capture, seconds")
    p.add_argument("--image-bytes", type=int, default=12000)
    p.add_argument("--queue-results", type=int, default=32, help="results the device queue holds")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    asyncio.run(main(a))
