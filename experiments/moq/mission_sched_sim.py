#!/usr/bin/env python3
"""Simulation of publisher scheduling for a counting mission over a variable link.

The device produces a result every inference period. A result may report passage events;
for such a result the device holds the source image, which it can send as evidence in one
of several representations. The link has a time-varying capacity and outages. A scheduler
decides, slice by slice, which object to send and in which representation.

The mission is a utility with three parts:
  C  share of the time in which the receiver knows the correct count
  E  evidence utility: for every result with events, the quality of the best representation
     that arrived within the evidence deadline (0 when none did)
  H  share of the results that arrived
Methods compared: priority and timeout with a fixed representation, representation by
throughput, representation by the deadline of one object, and the mission-utility scheduler
(MUS) with its variants.

The link is a queue served at the capacity. Results enter it at once; of the evidence object
under way at most a window of bytes is in it. What entered is carried even when its object
was abandoned. The model does not contain QUIC: no loss, no congestion control."""

import argparse
import ctypes
import json
import math
import random
import statistics
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MUS_EVENTS, MUS_REPS = 8, 3


class MusConfig(ctypes.Structure):
    _fields_ = [("quality", ctypes.c_uint32 * MUS_REPS), ("prepare_us", ctypes.c_uint32 * MUS_REPS),
                ("margin_permille", ctypes.c_uint32), ("guard_us", ctypes.c_uint32), ("probe_interval_us", ctypes.c_uint32),
                ("keep_permille", ctypes.c_uint32), ("sequential", ctypes.c_bool)]


class MusEvent(ctypes.Structure):
    _fields_ = [("seq", ctypes.c_uint32), ("deadline_us", ctypes.c_uint64), ("size", ctypes.c_size_t * MUS_REPS),
                ("best", ctypes.c_int8), ("active", ctypes.c_int8), ("prepared", ctypes.c_int8),
                ("converting", ctypes.c_int8), ("acked", ctypes.c_size_t), ("unacked", ctypes.c_size_t),
                ("probe", ctypes.c_bool)]


class MusLink(ctypes.Structure):
    _fields_ = [("rate_Bps", ctypes.c_uint64), ("deviation_Bps", ctypes.c_uint64), ("drain", ctypes.c_size_t),
                ("codec_free_us", ctypes.c_uint64)]


class MusDecision(ctypes.Structure):
    _fields_ = [("action", ctypes.c_int), ("event", ctypes.c_int), ("rep", ctypes.c_int), ("probe", ctypes.c_bool),
                ("prepare_event", ctypes.c_int), ("prepare_rep", ctypes.c_int), ("plan", ctypes.c_int8 * MUS_EVENTS)]


MUS_IDLE, MUS_CONTINUE, MUS_START, MUS_ABORT = range(4)
_planner = None


def planner():
    """The scheduler of the firmware (src/main/moq_mission_sched.c), built for the host."""
    global _planner
    if _planner is None:
        out = Path(tempfile.mkdtemp()) / "mus.so"
        src = ROOT / "src/main"
        subprocess.run(["cc", "-O2", "-shared", "-fPIC", f"-I{src}", str(src / "moq_mission_sched.c"), "-o", str(out)],
                       check=True)
        _planner = ctypes.CDLL(str(out))
        _planner.mus_plan.restype = MusDecision
        _planner.mus_plan.argtypes = [ctypes.POINTER(MusConfig), ctypes.POINTER(MusEvent), ctypes.c_uint, ctypes.c_uint64,
                                      ctypes.POINTER(MusLink), ctypes.c_uint64]
    return _planner

LADDER = [("low", 7100, 0.0), ("medium", 24780, 0.0), ("high", 87454, 1.0)]  # name, bytes on the device, quality
PREPARE = [1.8, 2.7, 0.0]  # seconds to produce a representation on the device; the source needs none
RESULT_BYTES = 420
SLICE = 1024


def ladder(qualities):
    return [(n, b, q) for (n, b, _), q in zip(LADDER, qualities)]


# -- link ---------------------------------------------------------------------------------
def link_constant(kbit):
    return lambda t: kbit * 125.0


def link_markov(seed, duration, states=(512, 128, 48, 0), dwell=(30, 20, 15, 8)):
    """Piecewise-constant capacity; exponential dwell times; returns bytes/s at time t."""
    rng, t, pieces = random.Random(seed), 0.0, []
    state = 0
    while t < duration + 60:
        hold = max(2.0, rng.expovariate(1.0 / dwell[state]))
        pieces.append((t, t + hold, states[state] * 125.0))
        t += hold
        state = rng.choice([s for s in range(len(states)) if s != state])
    def capacity(at):
        for a, b, c in pieces:
            if a <= at < b:
                return c
        return pieces[-1][2]
    capacity.pieces = pieces
    return capacity


# -- workload -----------------------------------------------------------------------------
class Item:
    __slots__ = ("kind", "seq", "born", "deadline", "rep", "size", "sent", "acked", "done_at", "new", "total", "aborted",
                 "probe")

    def __init__(self, kind, seq, born, deadline, size, rep=None, new=0, total=0):
        self.kind, self.seq, self.born, self.deadline = kind, seq, born, deadline
        self.rep, self.size, self.sent, self.acked, self.done_at = rep, size, 0, 0, None
        self.new, self.total, self.aborted, self.probe = new, total, False, False


def link_trace(points):
    """Piecewise-constant capacity from (time, kbit/s) points, as the shaper of the device runs."""
    def capacity(at):
        rate = points[0][1]
        for t, r in points:
            if t <= at:
                rate = r
        return rate * 125.0
    return capacity


DEVICE_TRACE = [(0, 192), (20, 64), (45, 0), (57, 128), (80, 48), (100, 192)]


def workload_regular(seed, duration, period):
    """The workload of the device runs: an event in every second and every third result in turn."""
    rng, t, seq, total, out = random.Random(seed * 101 + 7), 0.0, 0, 0, []
    gaps, since, k = (2, 3), 0, 0
    while t < duration:
        t += period * rng.uniform(0.98, 1.02)
        seq += 1
        since += 1
        new = 0
        if seq == 1 or since == gaps[k % 2]:
            new, since = 1, 0
            k += seq > 1
        total += new
        out.append((t, seq, new, total))
    return out


def workload(seed, duration, period, event_probability, burst=0.0):
    """`burst`: probability that a result reports an event when the previous one did. With 0
    the events are independent; otherwise they come in groups with the same mean rate."""
    rng, t, seq, total, out = random.Random(seed * 101 + 7), 0.0, 0, 0, []
    previous = False
    # p(event | no event) that keeps the mean rate
    idle = event_probability * (1 - burst) / (1 - event_probability) if burst else event_probability
    while t < duration:
        t += period * rng.uniform(0.95, 1.05)
        seq += 1
        p = (burst if previous else idle) if burst else event_probability
        new = 1 if rng.random() < p else 0
        previous = bool(new)
        total += new
        out.append((t, seq, new, total))
    return out


# -- device ---------------------------------------------------------------------------------
class Rate:
    """Service rate of the evidence objects, as estimator 2 of the firmware: bytes acknowledged
    in windows of one second while an object is under way; mean and mean deviation."""

    def __init__(self):
        self.mean, self.deviation, self.samples = 64000.0, 32000.0, 0
        self.start, self.bytes = None, 0.0

    def observe(self, sample):
        sample = min(4e6, max(1000.0, sample))
        if not self.samples:
            self.mean, self.deviation = sample, sample / 4
        else:
            self.deviation = (3 * self.deviation + abs(sample - self.mean)) / 4
            self.mean = (self.mean + sample) / 2 if sample < self.mean else (3 * self.mean + sample) / 4
        self.samples += 1

    def opened(self, t):
        if self.start is None:
            self.start, self.first, self.bytes, self.total, self.sampled = t, t, 0.0, 0.0, False

    def acked(self, n, t):
        if self.start is None:
            return
        self.bytes += n
        self.total += n
        if t - self.start >= 1.0:
            self.observe(self.bytes / (t - self.start))
            self.start, self.bytes, self.sampled = t, 0.0, True

    def closed(self, t):
        if self.start is None:
            return
        if t - self.start >= 0.5 and self.bytes:
            self.observe(self.bytes / (t - self.start))
        elif not self.sampled and self.total and t - self.first >= 0.2:
            self.observe(self.total / (t - self.first))
        self.start = None


class Device:
    """Sender. Results enter the network at once. One evidence object is under way at a time;
    at most `window` bytes of it are in the network and not acknowledged."""

    def __init__(self, policy, reps, *, queue=32, sources=4, evidence_deadline=10.0, detect=2.8, fixed="high",
                 margin=0.8, risk=False, sequential=False, state_delay=0.0, prepare=PREPARE, oracle=None,
                 latest=False, interval=6.7, window=32768):
        self.policy, self.reps = policy, reps
        self.queue, self.sources, self.window, self.state_delay = queue, sources, window, state_delay
        self.D, self.detect, self.margin, self.prepare, self.risk = evidence_deadline, detect, margin, list(prepare), risk
        self.config = MusConfig((ctypes.c_uint32 * 3)(*[round(1000 * q) for _, _, q in reps]),
                                (ctypes.c_uint32 * 3)(*[round(1e6 * x) for x in prepare]), round(1000 * margin), 100000,
                                2000000, 50, sequential)
        self.fixed = [n for n, _, _ in reps].index(fixed)
        self.results = []     # results that wait for the network, oldest first
        self.events = []      # events whose source is held
        self.rate = Rate()
        self.stats = dict(result_overflow=0, source_evicted=0, evidence_expired=0, evidence_aborted=0, wasted_bytes=0,
                          bytes_results=0, bytes_evidence=0, probes=0, prepared=0, discarded=0)
        self.probed = 0.0
        self.oracle, self.latest, self.interval = oracle, latest, interval
        self.current = None   # evidence object under way
        self.codec = None     # dict(event, rep, done): representation in production
        self.network = []     # [item, bytes] in the order of transmission
        self.drain = 0.0      # bytes of abandoned evidence in the network

    def produce(self, t, seq, new, total):
        item = Item("result", seq, t, math.inf, RESULT_BYTES, new=new, total=total)
        if len(self.results) >= self.queue:
            self.stats["result_overflow"] += 1
        else:
            self.results.append(item)
        if new:
            if len(self.events) >= self.sources:
                free = [e for e in self.events if e["item"] is None and e["converting"] < 0]
                if free:
                    gone = free[0]
                    self.events.remove(gone)
                    if gone["best"] < 0:
                        self.stats["source_evicted"] += 1
                else:
                    return
            self.events.append(dict(seq=seq, born=t - self.detect, best=-1, item=None, prepared=-1, converting=-1,
                                    probe=False, closed=False))

    def deadline(self, e):
        return e["born"] + self.D

    def abandon(self, item, t, counter):
        self.stats[counter] += 1
        self.stats["wasted_bytes"] += item.sent
        item.aborted = True
        self.drain += item.sent - item.acked
        if self.drain <= 1e-3:
            self.drain = 0.0
        if self.current is item:
            self.current = None
        for e in self.events:
            if e["item"] is item:
                e["item"] = None
        if self.drain == 0.0:
            self.rate.closed(t)

    def start(self, event, rep, t, probe=False):
        _, size, _ = self.reps[rep]
        item = Item("evidence", event["seq"], event["born"], self.deadline(event), size, rep=rep)
        item.probe = probe
        event["item"] = self.current = item
        self.rate.opened(t)
        return item

    # ---- the mission-utility scheduler
    def plan(self, t):
        if self.codec is not None and self.codec["done"] <= t:
            e, rep = self.codec["event"], self.codec["rep"]
            e["converting"], self.codec = -1, None
            if e in self.events:
                e["prepared"] = rep
                self.stats["prepared"] += 1
        for e in list(self.events):
            if e["item"] is None and e["converting"] < 0 and self.deadline(e) <= t:
                self.events.remove(e)
        events = [e for e in self.events if not e["closed"]][-MUS_EVENTS:]
        arr = (MusEvent * max(1, len(events)))()
        for k, e in enumerate(events):
            item = e["item"]
            arr[k] = MusEvent(e["seq"], round(1e6 * self.deadline(e)), (ctypes.c_size_t * 3)(*[b for _, b, _ in self.reps]),
                              e["best"], item.rep if item is not None else -1, e["prepared"], e["converting"],
                              round(item.acked) if item is not None else 0,
                              round(item.sent - item.acked) if item is not None else 0, e["probe"])
        mean, deviation = self.rate.mean, max(self.rate.deviation, self.rate.mean / 20) if self.risk else 0.0
        if self.oracle is not None:
            # bound: the mean capacity until the last open deadline is known
            mean, deviation = sum(self.oracle(t + k * 0.5) for k in range(14)) / 14, 0.0
        link = MusLink(round(mean), round(deviation), round(self.drain),
                       round(1e6 * (self.codec["done"] if self.codec is not None else t)))
        d = planner().mus_plan(ctypes.byref(self.config), arr, len(events), round(1e6 * t), ctypes.byref(link),
                               round(1e6 * self.probed) if self.probed > 0 else 0)
        e = events[d.event] if d.event >= 0 else None
        if d.action == MUS_ABORT:
            self.abandon(self.current, t, "evidence_aborted")
        if d.action == MUS_START:
            if d.probe and not e["probe"]:
                e["probe"], self.probed = True, t
                self.stats["probes"] += 1
            source = self.prepare[d.rep] == 0
            if (source or e["prepared"] == d.rep) and self.current is None and self.drain <= 0:
                self.start(e, d.rep, t, probe=e["probe"])
                if not source:
                    e["prepared"] = -1
        target = d.prepare_event
        if self.config.sequential and (self.current is not None or d.action != MUS_START or target != d.event):
            target = -1
        if target >= 0 and self.codec is None:
            for k, o in enumerate(events):
                if o["prepared"] >= 0 and not o["probe"] and d.plan[k] != o["prepared"]:
                    o["prepared"] = -1
                    self.stats["discarded"] += 1
            if all(o["prepared"] < 0 for o in self.events):
                o = events[target]
                o["converting"] = d.prepare_rep
                self.codec = dict(event=o, rep=d.prepare_rep, done=t + self.prepare[d.prepare_rep])

    # ---- the other methods: one representation rule, the latest source
    def choose(self, t):
        p = self.policy
        cur = self.current
        if cur is not None and p in ("timeout", "feasible") and t >= cur.deadline:
            self.abandon(cur, t, "evidence_expired")
        if self.codec is not None and self.codec["done"] <= t:
            e, rep = self.codec["event"], self.codec["rep"]
            self.codec = None
            e["converting"] = -1
            if e in self.events and not (p in ("timeout", "feasible") and t >= self.deadline(e)):
                e["prepared"] = rep
        if self.current is not None or self.drain > 0:
            return
        ready = next((e for e in self.events if e["prepared"] >= 0), None)
        if ready is not None:
            self.start(ready, ready["prepared"], t)
            ready["prepared"] = -1
            return
        if self.codec is not None:
            return
        pending = [e for e in self.events if e["best"] < 0 and e["item"] is None and not e["closed"]]
        if p in ("timeout", "feasible"):
            for e in list(pending):
                if t >= self.deadline(e):
                    e["closed"] = True
                    self.stats["evidence_expired"] += 1
                    pending.remove(e)
        if not pending:
            return
        e = pending[-1] if self.latest else pending[0]
        usable = self.margin * self.rate.mean
        if p == "abr":
            # throughput rule: the largest representation whose mean rate fits the estimate
            rep = max([r for r in range(len(self.reps)) if self.reps[r][1] / self.interval <= usable] or [0])
        elif p == "feasible":
            # the largest representation that reaches the receiver before the deadline of this object
            rep = max([r for r in range(len(self.reps))
                       if t + self.prepare[r] + self.reps[r][1] / usable + 0.1 <= self.deadline(e)] or [0])
        else:
            rep = self.fixed
        if self.latest:
            for old in pending[:-1]:
                old["closed"] = True     # replaced by a newer source before it was sent
        if self.prepare[rep] == 0:
            self.start(e, rep, t)
        else:
            e["converting"] = rep
            self.codec = dict(event=e, rep=rep, done=t + self.prepare[rep])

    def send(self, t):
        """What enters the network at time t."""
        if self.policy == "mus":
            self.plan(t)
            self.results.reverse()        # the newest result carries the count
        else:
            self.choose(t)
        held = self.policy == "default" and self.current is not None
        for r in list(self.results):
            if held and self.current.born <= r.born and self.current.sent < self.current.size:
                break                     # no priority: the object that came first goes first
            self.results.remove(r)
            r.sent = r.size
            self.network.append([r, r.size])
            self.stats["bytes_results"] += r.size
        if self.policy == "mus":
            self.results.reverse()
        cur = self.current
        if cur is not None and cur.sent < cur.size:
            window = self.window
            if self.state_delay:
                # a state result waits behind the evidence in the network for at most this delay
                window = min(window, max(4096.0, self.rate.mean * self.state_delay))
            n = min(cur.size - cur.sent, window - (cur.sent - cur.acked))
            if n > 0:
                cur.sent += n
                self.network.append([cur, n])
                self.stats["bytes_evidence"] += n


def simulate(policy, capacity, produced, reps, *, duration, dt=0.02, delay=0.05, oracle=False, **options):
    d = Device(policy, reps, oracle=capacity if oracle else None, **options)
    arrivals, evidence = [], {}   # (time, total, new, seq) ; seq -> (quality, time)
    k, t = 0, 0.0
    end = duration + 60.0
    while t < end:
        while k < len(produced) and produced[k][0] <= t:
            d.produce(*produced[k])
            k += 1
        d.send(t)
        budget = capacity(t) * dt
        while budget > 0 and d.network:
            item, n = d.network[0]
            take = min(budget, n)
            budget -= take
            d.network[0][1] -= take
            if d.network[0][1] <= 0:
                d.network.pop(0)
            if item.kind == "result":
                item.acked += take
                if item.acked >= item.size - 1e-3:
                    arrivals.append((t + delay, item.total, item.new, item.seq))
                continue
            item.acked += take
            d.rate.acked(take, t + dt)
            if item.aborted:
                d.drain -= take
                if d.drain <= 1e-3:
                    d.drain = 0.0
                    if d.current is None:
                        d.rate.closed(t + dt)
                continue
            if item.acked >= item.size - 1e-3:
                item.done_at = t + delay
                e = next((x for x in d.events if x["item"] is item), None)
                if e is not None:
                    e["item"], e["probe"] = None, False
                    if item.done_at <= item.deadline:
                        e["best"] = max(e["best"], item.rep)
                if item.done_at <= item.deadline:
                    q = reps[item.rep][2]
                    if q > evidence.get(item.seq, (0.0, 0.0))[0]:
                        evidence[item.seq] = (q, item.done_at)
                d.current = None
                d.rate.closed(t + dt)
        t += dt
    return d, arrivals, evidence


def utility(produced, arrivals, evidence, duration, weights):
    truth = [(t, total) for t, _, _, total in produced]
    known, c = [], 0
    for t, total, _, _ in sorted(arrivals):
        c = max(c, total)
        known.append((t, c))
    points = sorted({0.0, duration, *[t for t, _ in truth if t < duration], *[t for t, _ in known if t < duration]})
    def at(series, x):
        v = 0
        for u, w in series:
            if u <= x:
                v = w
            else:
                break
        return v
    wrong = sum(b - a for a, b in zip(points, points[1:]) if at(truth, a) != at(known, a))
    events = [seq for _, seq, new, _ in produced if new]
    C = 1.0 - wrong / duration
    E = statistics.mean(evidence.get(s, (0.0, 0.0))[0] for s in events) if events else 1.0
    H = len({s for _, _, _, s in arrivals}) / len(produced)
    covered = sum(1 for s in events if s in evidence) / len(events) if events else 1.0
    wc, we, wh = weights
    return dict(C=C, E=E, H=H, U=wc * C + we * E + wh * H, evidence_covered=covered,
                count_error=(truth[-1][1] if truth else 0) - (known[-1][1] if known else 0))


# The other methods keep the latest source only, as the firmware does. Results have no timeout.
POLICIES = dict(
    priority_high=dict(policy="priority", fixed="high", latest=True),
    priority_medium=dict(policy="priority", fixed="medium", latest=True),
    timeout_high=dict(policy="timeout", fixed="high", latest=True),
    timeout_medium=dict(policy="timeout", fixed="medium", latest=True),
    abr=dict(policy="abr", latest=True),
    feasible=dict(policy="feasible", latest=True),
    mus=dict(policy="mus", margin=1.0, risk=True, state_delay=1.0),
    mus_norisk=dict(policy="mus", margin=0.8, risk=False, state_delay=1.0),
    mus_noahead=dict(policy="mus", margin=1.0, risk=True, sequential=True, state_delay=1.0),
    mus_unbounded=dict(policy="mus", margin=1.0, risk=True),
    mus_oracle=dict(policy="mus", margin=1.0, oracle=True, state_delay=1.0),
)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--duration", type=float, default=600.0)
    p.add_argument("--period", type=float, default=2.8)
    p.add_argument("--events", type=float, default=0.42, help="probability that a result reports a passage event")
    p.add_argument("--burst", type=float, default=0.0, help="probability of an event after an event; 0: independent")
    p.add_argument("--seeds", type=int, default=20)
    p.add_argument("--qualities", type=float, nargs=3, default=[0.115, 0.491, 1.0],
                   help="quality of low, medium, high; default: measured with the reference detector")
    p.add_argument("--weights", type=float, nargs=3, default=[0.4, 0.5, 0.1], help="count, evidence, history")
    p.add_argument("--deadline", type=float, default=10.0)
    p.add_argument("--queue", type=int, default=32)
    p.add_argument("--sources", type=int, default=4)
    p.add_argument("--detect", type=float, default=2.8, help="time from the capture of the source to its result")
    p.add_argument("--efficiency", type=float, default=0.87, help="share of the link rate that carries payload")
    p.add_argument("--links", type=int, nargs="+", default=[32, 48, 64, 96, 128, 192, 256])
    p.add_argument("--device", action="store_true", help="workload and variable link of the device runs")
    a = p.parse_args()
    reps = ladder(a.qualities)
    scale = lambda f: (lambda t: a.efficiency * f(t))  # noqa: E731
    links = {f"{k} kbit/s": (lambda seed, k=k: scale(link_constant(k))) for k in a.links}
    links["variable"] = lambda seed: scale(link_markov(seed, a.duration, states=(192, 96, 48, 0), dwell=(25, 25, 20, 8)))
    if a.device:
        links["variable"] = lambda seed: scale(link_trace(DEVICE_TRACE))
    rows = []
    for link_name, make in links.items():
        for name, options in POLICIES.items():
            runs = []
            for seed in range(1, a.seeds + 1):
                produced = workload_regular(seed, a.duration, a.period) if a.device else \
                    workload(seed, a.duration, a.period, a.events, a.burst)
                d, arrivals, evidence = simulate(capacity=make(seed), produced=produced, reps=reps, duration=a.duration,
                                                 queue=a.queue, sources=a.sources, evidence_deadline=a.deadline,
                                                 detect=a.detect, **options)
                u = utility(produced, arrivals, evidence, a.duration, a.weights)
                u.update(seed=seed, stats=d.stats)
                runs.append(u)
            m = lambda k: statistics.mean(r[k] for r in runs)  # noqa: E731
            rows.append(dict(link=link_name, policy=name, runs=len(runs), U=m("U"), C=m("C"), E=m("E"), H=m("H"),
                             covered=m("evidence_covered"), U_min=min(r["U"] for r in runs), U_max=max(r["U"] for r in runs),
                             wasted_kB=statistics.mean(r["stats"]["wasted_bytes"] for r in runs) / 1000,
                             count_error=sum(r["count_error"] for r in runs), cells=runs))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "summary.json").write_text(json.dumps(dict(schema=1, kind="fluid simulation", options=vars(a) | dict(out=str(a.out)),
                                                        rows=rows), indent=1) + "\n")
    for link_name in links:
        print(f"\n{link_name}    U      C      E      H   covered  wasted kB")
        for r in (x for x in rows if x["link"] == link_name):
            print(f"  {r['policy']:16s} {r['U']:.3f}  {r['C']:.3f}  {r['E']:.3f}  {r['H']:.3f}  {r['covered']:.3f}  {r['wasted_kB']:7.1f}")


if __name__ == "__main__":
    main()
