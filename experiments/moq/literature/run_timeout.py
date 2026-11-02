#!/usr/bin/env python3
"""Author timeout path on loopback QUIC with a bounded UDP payload-rate bottleneck.

Five independently decodable, disjoint JPEG tiles adapt the multiple-description
concept. This is not the paper's Draco point-cloud codec or PCQM evaluation.
"""

import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import random
import time

from PIL import Image

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Bottleneck(asyncio.DatagramProtocol):
    def __init__(self, server, rate):
        self.server = server
        self.rate = rate * 1000 / 8
        self.client = None
        self.queue = asyncio.Queue(maxsize=64)
        self.stats = dict(
            server_packets=0,
            server_bytes=0,
            forwarded_packets=0,
            forwarded_bytes=0,
            queue_drops=0,
            max_queue=0,
        )
        self.due = 0

    def connection_made(self, transport):
        self.transport = transport
        self.worker = asyncio.create_task(self.forward())

    def datagram_received(self, data, address):
        if address == self.server:
            self.stats["server_packets"] += 1
            self.stats["server_bytes"] += len(data)
            if self.queue.full():
                self.stats["queue_drops"] += 1
                return
            now = time.monotonic()
            self.due = max(self.due, now) + len(data) / self.rate
            self.queue.put_nowait((self.due + 0.010, bytes(data)))
            self.stats["max_queue"] = max(self.stats["max_queue"], self.queue.qsize())
        elif self.client is None or address == self.client:
            self.client = address
            self.transport.sendto(data, self.server)

    async def forward(self):
        while True:
            due, data = await self.queue.get()
            await asyncio.sleep(max(0, due - time.monotonic()))
            if self.client is not None:
                self.transport.sendto(data, self.client)
                self.stats["forwarded_packets"] += 1
                self.stats["forwarded_bytes"] += len(data)


def make_assets(out):
    assets = out / "tiles"
    assets.mkdir()
    source = ROOT / "experiments/moq/fixtures/screw-ladder-v1/high.jpg"
    with Image.open(source) as image:
        image = image.convert("RGB").resize((400, 300), Image.Resampling.LANCZOS)
        for tile in range(5):
            image.crop((0, tile * 60, 400, (tile + 1) * 60)).save(
                assets / f"tile-{tile}.jpg", quality=75
            )
    return assets, dict(
        source_sha256=sha(source),
        width=400,
        height=300,
        encoding="Pillow JPEG quality75, five horizontal 400x60 independent tiles",
        tiles={
            p.name: dict(sha256=sha(p), bytes=p.stat().st_size)
            for p in sorted(assets.glob("*.jpg"))
        },
    )


async def main(args):
    out = BASE / "results" / args.name
    out.mkdir(exist_ok=False)
    assets, asset_manifest = make_assets(out)
    executable = args.binary.resolve()
    cells = []
    for repeat in range(1, 2 if args.smoke else 4):
        block = [
            dict(timeout_ms=t, group_frames=g, rate_kbps=r, repeat=repeat)
            for t in ([0, 100] if args.smoke else [0, 100, 500])
            for g in ([5] if args.smoke else [5, 30])
            for r in ([128] if args.smoke else [128, 512])
        ]
        random.Random(20260924 + repeat).shuffle(block)
        cells.extend(block)
    manifest = dict(
        scope=__doc__,
        upstream_commit="f18fae1f520f074bd61344bbe7ee07530a879e08",
        executable_sha256=sha(executable),
        assets=asset_manifest,
        producer=dict(frames=60, hz=10, descriptions=5, chunk_bytes=1024),
        bottleneck=dict(queue_packets=64, delay_ms=10, units="UDP payload bytes; no IP overhead"),
        send_window_bytes=args.send_window_bytes,
        cells=cells,
        results=[],
        status="running",
    )

    def save():
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    save()
    try:
        for index, cell in enumerate(cells):
            if sha(executable) != manifest["executable_sha256"]:
                raise RuntimeError("timeout executable changed after freezing suite")
            name = f"{index:02d}-t{cell['timeout_ms']}-g{cell['group_frames']}-r{cell['rate_kbps']}"
            proxy = Bottleneck(("127.0.0.1", 15481), cell["rate_kbps"])
            transport, _ = await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: proxy, local_addr=("127.0.0.1", 15482)
            )
            process = None
            try:
                process = await asyncio.create_subprocess_exec(
                    str(executable),
                    "15481",
                    "15482",
                    str(cell["timeout_ms"]),
                    str(cell["group_frames"]),
                    str(assets),
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                    env={**os.environ, **({"LITERATURE_QUIC_SEND_WINDOW": str(args.send_window_bytes)}
                                         if args.send_window_bytes is not None else {})},
                )
                stdout, stderr = await asyncio.wait_for(process.communicate(), timeout=40)
                (out / (name + ".stdout.log")).write_bytes(stdout)
                (out / (name + ".stderr.log")).write_bytes(stderr)
                if process.returncode:
                    raise RuntimeError(f"{name} failed {process.returncode}: {stderr[-500:]!r}")
                line = next(
                    s for s in stdout.decode().splitlines() if s.startswith("LITERATURE_RESULT ")
                )
                result = json.loads(line.split(" ", 1)[1])
                result["bottleneck"] = proxy.stats
                (out / (name + ".json")).write_text(json.dumps(result, indent=2) + "\n")
                record = dict(
                    **cell,
                    status="complete",
                    file=name + ".json",
                    descriptions=sum(e["event"] == "description" for e in result["events"]),
                )
                manifest["results"].append(record)
                save()
                print(json.dumps(record), flush=True)
            finally:
                if process is not None and process.returncode is None:
                    process.terminate()
                    try:
                        await asyncio.wait_for(process.wait(), timeout=5)
                    except asyncio.TimeoutError:
                        process.kill()
                        await process.wait()
                proxy.worker.cancel()
                transport.close()
                await asyncio.gather(proxy.worker, return_exceptions=True)
        manifest["status"] = "complete"
    except BaseException as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        raise
    finally:
        save()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--send-window-bytes", type=int)
    parser.add_argument("--binary", type=Path,
                        default=BASE / "timeout_client/target/debug/literature-timeout-client")
    asyncio.run(main(parser.parse_args()))
