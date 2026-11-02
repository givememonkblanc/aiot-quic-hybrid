#!/usr/bin/env python3
"""Finite actual-QUIC author relay trials, loopback only; no system tc changes."""

import argparse, hashlib, json, random, subprocess, time
from pathlib import Path

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[2]


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--name", required=True)
    p.add_argument("--smoke", action="store_true")
    p.add_argument("--reference", choices=["draft16", "draft18"], default="draft16")
    a = p.parse_args()
    vendor = BASE / "vendor" / ("moqtail-draft16" if a.reference == "draft16" else "moqtail")
    client_name = "track16_client" if a.reference == "draft16" else "track_client"
    binary_name = (
        "literature-track16-client" if a.reference == "draft16" else "literature-track-client"
    )
    client = BASE / client_name / "target/debug" / binary_name
    config = (vendor / "apps/relay/src/server/config.rs").read_text()
    if "Ipv6Addr::LOCALHOST, port" not in config:
        p.error("apply the recorded loopback-only relay patch before running")
    out = BASE / "results" / a.name
    out.mkdir(exist_ok=False)
    cert = out / "cert.pem"
    key = out / "key.pem"
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-days",
            "2",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost,IP:::1",
        ],
        check=True,
        capture_output=True,
    )
    key.chmod(0o600)
    medium = ROOT / "experiments/moq/fixtures/screw-ladder-v1/medium.jpg"
    low = medium.with_name("low.jpg")
    cells = []
    for repeat in range(1, 2 if a.smoke else 4):
        block = [
            dict(mode=m, delay_ms=d, rate_kbps=r, repeat=repeat)
            for r in ([0] if a.smoke else [0, 1024])
            for m, d in [("atomic", 0), ("separate", 0), ("separate", 200), ("separate", 800)]
        ]
        random.Random(20260924 + repeat).shuffle(block)
        cells.extend(block)
    manifest = dict(
        scope="original author relay switching; aligned JPEG groups; application write cap not network bandwidth",
        upstream_commit=subprocess.check_output(
            ["git", "-C", str(vendor), "rev-parse", "HEAD"], text=True
        ).strip(),
        reference=a.reference,
        relay_patch="IPv6 ANY binding changed to LOOPBACK; client IPv6 authority normalization; no algorithm modifications",
        relay_sha256=sha(vendor / "target/debug/relay"),
        client_sha256=sha(client),
        input_hashes={p.name: sha(p) for p in (medium, low)},
        cells=cells,
        results=[],
        status="running",
    )

    def save():
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    save()
    try:
        for index, cell in enumerate(cells):
            if (
                sha(client) != manifest["client_sha256"]
                or sha(vendor / "target/debug/relay") != manifest["relay_sha256"]
            ):
                raise RuntimeError("reference executable changed after freezing suite")
            name = f"{index:02d}-{cell['mode']}-{cell['delay_ms']}-{cell['rate_kbps']}"
            port = 15473
            with (out / (name + "-relay.log")).open("w") as log:
                relay = subprocess.Popen(
                    [
                        str(vendor / "target/debug/relay"),
                        "--port",
                        str(port),
                        "--cert-file",
                        str(cert),
                        "--key-file",
                        str(key),
                        "--write-kbps-limit",
                        str(cell["rate_kbps"]),
                        "--log-folder",
                        str(out),
                    ],
                    cwd=vendor,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                try:
                    time.sleep(0.5)
                    if relay.poll() is not None:
                        raise RuntimeError("relay startup failed")
                    result = subprocess.run(
                        [
                            str(client),
                            cell["mode"],
                            str(cell["delay_ms"]),
                            str(port),
                            str(medium),
                            str(low),
                        ],
                        capture_output=True,
                        text=True,
                        timeout=35,
                    )
                    (out / (name + "-stderr.log")).write_text(result.stderr)
                    if result.returncode:
                        raise RuntimeError(f"client failed {name}: {result.stderr[-1000:]}")
                    data = json.loads(result.stdout)
                    (out / (name + ".json")).write_text(json.dumps(data, indent=2) + "\n")
                    record = dict(
                        **cell, file=name + ".json", objects=len(data["objects"]), status="complete"
                    )
                    manifest["results"].append(record)
                    save()
                    print(json.dumps(record), flush=True)
                finally:
                    relay.terminate()
                    try:
                        relay.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        relay.kill()
                        relay.wait()
        manifest["status"] = "complete"
    except BaseException as exc:
        manifest["status"] = "failed"
        manifest["error"] = str(exc)
        raise
    finally:
        save()


if __name__ == "__main__":
    main()
