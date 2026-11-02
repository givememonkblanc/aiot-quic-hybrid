#!/usr/bin/env python3
"""Model device of the emulation (emu_trial.py): the workload of a recorded ESP32 run, replayed
over a real QUIC connection by the publisher model of mission_device.py, with the evidence
policies of the device matrix.

Workload. The inferences of a recorded run (serial.log of an ESP32 run): for each, the
production time (start + duration of the inference), its capture, the passages it reports and,
for a passage, the size of its evidence. Transformations, all seeded:
  --scale k      the timeline is compressed by k (k passages per passage time; inference
                 periods stay those of the device: a compressed timeline keeps every k-th gap)
  --cluster n    the passages are moved to the nearest of clusters of n consecutive passages
  --fp p         each result without a passage reports a false passage with probability p

Policies (the device side of tools/build_matrix.sh):
  prio-to-low    results first, the latest evidence only (a newer one replaces it), at most
                 32 KiB of evidence unacknowledged, evidence expires 10 s after its capture
  prio-to        the same at the medium representation
  mbm-floor      results first, up to 8 events hold their evidence (MUS_EVENTS) and are sent in
                 deadline order at the low representation, the evidence in the network is bounded
                 by W = max(1200, rho * tau1) (rho: acknowledged evidence bytes over windows of one
                 second while evidence is unacknowledged, tau1 = 100 ms), and a PING every 50 ms
                 while a result is unacknowledged
  mbm-floor-bdp  mbm-floor with W = max(1200, rho * (tau1 + rtt_min))

The device writes serial.log in the format of the ESP32 serial record (wall time, [TAG], JSON),
with a device clock that starts at the boot of the model, so that the analysis of device runs
(analyze_evidence_utility.py, analyze_matrix.py) applies unchanged. This is a model: it shows how
the policies behave under the same load and link; the radio and the MCU are not modelled."""
import argparse
import asyncio
import json
import random
import time
from pathlib import Path

import moqt_wire as w
from mission_device import EVENTS, EVIDENCE, POLICIES, Device, jpeg

REP_LEVEL = {"low": 2, "medium": 1, "high": 0}   # property 126: 0 high, 1 medium, 2 low
EMU = {
    "prio-to-low": dict(rep="low", queue=1, window=None, probe=False),
    "prio-to": dict(rep="medium", queue=1, window=None, probe=False),
    "mbm-floor": dict(rep="low", queue=8, window="derived", probe=True),
    "mbm-floor-bdp": dict(rep="low", queue=8, window="bdp", probe=True),
}
TAU1, FLOOR_WINDOW, PROBE_S, EVIDENCE_S = 0.1, 1200, 0.05, 10.0


def load_trace(serial, ratios):
    """Inferences of a recorded run: [dict(t production s, capture s, seq, passages, sizes)]."""
    start, dur, cap, passages, evidence, out = {}, {}, {}, {}, {}, []
    for line in Path(serial).read_text(errors="ignore").splitlines():
        if "] " not in line or "[" not in line:
            continue
        tag = line.split("[", 1)[1].split("]", 1)[0]
        body = line.split("] ", 1)[1]
        if not body.startswith("{"):
            continue
        try:
            d = json.loads(body)
        except ValueError:
            continue
        if tag == "AI_START":
            start[d["seq"]], cap[d["seq"]] = d["start_us"] / 1e6, d["capture_ms"] / 1e3
        elif tag == "AI_PROFILE":
            dur[d["seq"]] = d["duration_us"] / 1e6
        elif tag == "MOQ_PASSAGE":
            passages[d["seq"]] = passages.get(d["seq"], 0) + 1
        elif tag == "MOQ_EVIDENCE":
            evidence[d["group"]] = d["bytes"]
    for seq in sorted(s for s in start if s in dur):
        high = evidence.get(seq, 64000)
        out.append(dict(seq=seq, t=start[seq] + dur[seq], capture=cap[seq], passages=passages.get(seq, 0),
                        sizes={k: int(high * r) for k, r in ratios.items()}))
    t0 = out[0]["t"]
    for x in out:
        x["t"] -= t0
        x["capture"] -= t0
    return out


def transform(trace, scale, cluster, fp, rng):
    """The workload variants of the AIoT experiments (see the module docstring)."""
    trace = [dict(x) for x in trace]
    if cluster > 1:
        idx = [i for i, x in enumerate(trace) if x["passages"]]
        for c in range(0, len(idx), cluster):
            group = idx[c:c + cluster]
            head = group[0]
            for i in group[1:]:
                trace[head]["passages"] += trace[i]["passages"]
                trace[i]["passages"] = 0
    if scale > 1:
        # k inferences of the recording fall into one inference period: their passages add up
        merged = []
        for i in range(0, len(trace), int(scale)):
            part = trace[i:i + int(scale)]
            m = dict(part[0])
            m["passages"] = sum(x["passages"] for x in part)
            merged.append(m)
        period = (trace[-1]["t"] - trace[0]["t"]) / max(1, len(trace) - 1)
        for k, m in enumerate(merged):
            lag = m["t"] - m["capture"]
            m["t"] = k * period
            m["capture"] = m["t"] - lag
        trace = merged
    if fp > 0:
        for x in trace:
            if not x["passages"] and rng.random() < fp:
                x["passages"], x["false"] = 1, True
    return trace


class EmuDevice(Device):
    def __init__(self, emu, trace, serial, *, run_s, **kw):
        super().__init__("D", produce_s=run_s, deadline_s=EVIDENCE_S, **kw)
        self.e = EMU[emu]
        self.emu = emu
        self.trace = trace
        self.serial = open(serial, "a", buffering=1)
        self.boot = time.monotonic()
        self.evidence = []            # events holding their evidence, deadline order
        self.rate, self.win = None, dict(start=None, acked=0)
        self.last_probe = 0.0
        self.log("MOQ_BOOT", dict(emu=emu))
        self.log("NETLOG", "emulated device")

    # -- serial record ----------------------------------------------------------
    def dev_us(self, t=None):
        return int(((time.monotonic() if t is None else t) - self.boot) * 1e6)

    def log(self, tag, data):
        body = json.dumps(data, separators=(",", ":")) if isinstance(data, dict) else data
        self.serial.write(f"{time.time():.3f} [{tag}] {body}\n")

    # -- workload ------------------------------------------------------------------
    async def producer(self, start):
        for k, x in enumerate(self.trace):
            if x["t"] >= self.produce_s:
                return
            await asyncio.sleep(max(0.0, start + x["t"] - time.monotonic()))
            now = time.monotonic()
            capture = start + x["capture"]
            self.log("AI_START", dict(seq=x["seq"], capture_ms=int(self.dev_us(capture) / 1000),
                                      start_us=self.dev_us(capture), priority=0))
            self.log("MOQ_CAPTURE", dict(seq=x["seq"], done_us=self.dev_us(capture), bytes=x["sizes"]["high"]))
            self.log("AI_PROFILE", dict(ts_ms=int(self.dev_us(capture) / 1000), seq=x["seq"],
                                        duration_us=self.dev_us(now) - self.dev_us(capture), detections=x["passages"]))
            self.log("MOQ_RESOURCE", dict(ts_us=self.dev_us(now)))
            self.emit(x, capture, now)

    def emit(self, x, capture, now):
        self.total += x["passages"]
        item = dict(group=x["seq"], capture_t=capture, passages_new=x["passages"], passages_total=self.total,
                    deadline=capture + EVIDENCE_S, size=x["sizes"][self.e["rep"]], false=x.get("false", False))
        for i in range(x["passages"]):
            self.log("MOQ_PASSAGE", dict(id=self.total - x["passages"] + i + 1, seq=x["seq"], x=0, y=0, direction=1))
        self.log("MOQ_AI_PRODUCED", dict(seq=x["seq"], source_ms=int(self.dev_us(capture) / 1000), enqueued=1))
        self.ledger.append(dict(item))
        self.results.append(item)
        if x["passages"]:
            self.log("MOQ_EVIDENCE", dict(group=x["seq"], bytes=x["sizes"]["high"], stored=1))
            self.stats["images_offered"] += 1
            if self.e["queue"] == 1:
                if self.pending_image is not None:
                    self.stats["images_replaced"] += 1
                self.pending_image = item
            else:
                if len(self.evidence) >= self.e["queue"]:
                    gone = self.evidence.pop(0)          # the oldest event gives up its evidence
                    self.log("MOQ_MUS_EVICT", dict(group=gone["group"], best=-1))
                    self.stats["images_replaced"] += 1
                self.evidence.append(item)

    # -- evidence ------------------------------------------------------------------
    def next_evidence(self, now):
        if self.e["queue"] == 1:
            item, self.pending_image = self.pending_image, None
            return item
        while self.evidence:
            item = min(self.evidence, key=lambda e: e["deadline"])
            self.evidence.remove(item)
            # a plan only sends what can arrive before its deadline at the estimated rate
            rate = self.rate or 8192
            if now + item["size"] / rate < item["deadline"]:
                return item
            self.stats["images_expired"] += 1
        return None

    def admit(self, pub, now):
        pending, self.pending_image = self.pending_image, None
        super().admit(pub, now)                         # results (the image part sees no pending image)
        self.pending_image = pending
        if self.slots["image"] is not None:
            return
        item = self.next_evidence(now)
        if item is None:
            return
        if now >= item["deadline"]:
            self.stats["images_expired"] += 1
            return
        data = self.image_of(item["size"])
        level = REP_LEVEL[self.e["rep"]]
        d = pub.publish(EVIDENCE, item["group"], data, props=w.kvp([(0x78, 1), (0x7E, level)]), retain=False, send=False)
        if d:
            pub.lib.moqt_core_stream_opened(pub.core, d[0]["sub"])
            self.open_slot(pub, "image", d[0]["head"], data, item, now)
            self.log("MOQ_REPRESENTATION", dict(group=item["group"], level=level, bytes=len(data), admission=0))

    _images = {}

    def image_of(self, size):
        key = max(1024, size // 512 * 512)
        if key not in self._images:
            self._images[key] = jpeg(key)
        return self._images[key]

    # -- window, rate and probe --------------------------------------------------
    def window(self, pub):
        if self.e["window"] is None:
            return self.unacked_image
        rate = self.rate or 8192
        wait = TAU1
        if self.e["window"] == "bdp":
            rtt = pub._quic._loss._rtt_min
            wait += rtt if rtt is not None else 0.0
        return max(FLOOR_WINDOW, int(rate * wait))

    def observe(self, pub, now):
        slot = self.slots["image"]
        sender = self.sender(pub, slot) if slot else None
        acked = sender._buffer_start if sender is not None else None
        busy = slot is not None and acked is not None and acked < len(slot.data)
        w0 = self.win
        if not busy:
            w0["start"] = None
            return
        if w0["start"] is None or w0.get("stream") != slot.stream:
            w0.update(start=now, acked=acked, stream=slot.stream)
            return
        if now - w0["start"] >= 1.0:
            sample = (acked - w0["acked"]) / (now - w0["start"])
            if sample > 0:
                self.rate = sample if self.rate is None else 0.75 * self.rate + 0.25 * sample
            w0.update(start=now, acked=acked)

    def serve(self, pub, now):
        self.observe(pub, now)
        self.unacked_image = self.window(pub)
        super().serve(pub, now)
        if self.e["probe"] and self.slots["result"] is not None and now - self.last_probe >= PROBE_S:
            sender = self.sender(pub, self.slots["result"])
            if sender is not None and sender._buffer_start < len(self.slots["result"].data):
                pub._quic.send_ping(int(now * 1000) & 0xFFFF)
                self.last_probe = now

    def stats_line(self):
        self.log("MOQ_EMU_STATS", dict(self.stats, rate_Bps=int(self.rate or 0)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--emu", required=True, choices=sorted(EMU))
    p.add_argument("--trace", type=Path, required=True, help="serial.log of a recorded ESP32 run")
    p.add_argument("--host", required=True)
    p.add_argument("--port", type=int, default=14568)
    p.add_argument("--cert", required=True)
    p.add_argument("--serial", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=213)
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--cluster", type=int, default=1)
    p.add_argument("--fp", type=float, default=0.0)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--ratios", default="1.0,0.502,0.124", help="bytes of high, medium, low per high byte")
    a = p.parse_args()
    hi, med, low = (float(x) for x in a.ratios.split(","))
    rng = random.Random(a.seed)
    trace = transform(load_trace(a.trace, dict(high=hi, medium=med, low=low)), a.scale, a.cluster, a.fp, rng)
    dev = EmuDevice(a.emu, trace, a.serial, run_s=a.seconds, cert=a.cert, host=a.host, port=a.port, seed=a.seed,
                    idle_timeout=20.0, attempt_s=5.0, drain_s=15.0)
    asyncio.run(dev.run())
    dev.stats_line()


if __name__ == "__main__":
    main()
