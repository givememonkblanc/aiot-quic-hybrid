#!/usr/bin/env python3
"""Receiver of a relay run (RELAY_EXPERIMENT.md), in place of receiver_draft21.py in
remote_ota_trial.py: the device connects to the MOQT relay (relay/moq_relay), and the
subscribers connect to the relay, each from its own network namespace behind an emulated link.

The run directory receives
  relay.jsonl          the log of the relay
  events.jsonl         the first hop in the format of the receivers: the subscriptions of the
                       relay, every object as it arrived at the relay, and "finished" when every
                       subscriber has finished
  sub-<name>/          the run of each subscriber (receiver_draft21.py --connect)
  links.json           the links of the subscribers and every outage applied, in monotonic time

A link is shaped in the direction of the data, relay to subscriber, as flow_shaper.py shapes the
device: rate, one-way delay with normal jitter, Gilbert-Elliott burst loss, a queue in packets,
and outages that drop every packet of that direction. Needs passwordless sudo for ip and tc.

  --sub NAME:LINK:TRACKS   LINK f128 | f512 | fwifi | trace | none | wan, TRACKS results | all;
                           wan: a subscriber on another machine over the Internet (wan_sub.py)
  --long NAME:START:DUR    an outage of the subscriber link that outlasts the QUIC idle timeout
  --neighbours N           N model devices (device_main.py, policy D) publish next to the device,
                           each to a relay of its own; their uplink is redirected into the IFB of
                           the first hop, so that they share its queue with the device. The
                           subscribers named by --neighbour-sub receive them over their own link.
  --neighbour-sub NAME:TRACKS  a subscriber that also receives every neighbour (results | all)"""
import argparse
import getpass
import json
import os
import random
import signal
import subprocess
import sys
import time
from pathlib import Path

from flow_shaper import DelayTrace, OutageSchedule, load_delay_trace
import wan_sub

BASE = Path(__file__).resolve().parent
PYTHON = BASE / "vendor/venv/bin/python"
# AIOT_NS_TAG: a tag in the names of the namespaces and interfaces, and another address range,
# so that an emulated run (emu_trial.py) does not meet the links of a device run on the same host.
NS_TAG = os.environ.get("AIOT_NS_TAG", "")
NET = "10.81" if NS_TAG else "10.79"
NB_NET = "10.82" if NS_TAG else "10.78"
LINKS = {
    "f128": dict(rate_kbit=128, delay_ms=5, jitter_ms=5, ge_loss=(1, 30, 50, 0), outages=True),
    "f512": dict(rate_kbit=512, delay_ms=5, jitter_ms=5, ge_loss=(1, 30, 50, 0), outages=True),
    "fwifi": dict(rate_kbit=20000, delay_ms=5, jitter_ms=5, ge_loss=(1, 30, 50, 0), outages=True),
    "trace": dict(rate_kbit=256, delay_ms=5, jitter_ms=0, ge_loss=None, outages=False),
    "none": dict(rate_kbit=1000000, delay_ms=0, jitter_ms=0, ge_loss=None, outages=False),
    "wan": dict(outages=False),
}


def sudo(*args, check=True):
    return subprocess.run(["sudo", "-n", *args], check=check, text=True, capture_output=True)


class SubLink:
    """veth pair to a namespace; the interface of FlowShaper for the schedules."""

    def __init__(self, index, rate_kbit, delay_ms, jitter_ms, ge_loss, queue_packets=64, **_):
        self.ns, self.host_if, self.sub_if = f"moqsub{NS_TAG}{index}", f"mqsh{NS_TAG}{index}", f"mqsd{NS_TAG}{index}"
        self.host_ip, self.sub_ip = f"{NET}.{index}.1", f"{NET}.{index}.2"
        self.rate_kbit, self.delay_ms, self.jitter_ms, self.ge_loss = rate_kbit, delay_ms, jitter_ms, ge_loss
        self.queue_packets = queue_packets
        self.blocked = False
        self.actions = []

    def netem_args(self, loss_percent=None):
        delay = [f"{self.delay_ms:g}ms"] + ([f"{self.jitter_ms}ms", "distribution", "normal"] if self.jitter_ms else [])
        if loss_percent is None and self.ge_loss is not None:
            loss = ["loss", "gemodel"] + [f"{x}%" for x in self.ge_loss]
        else:
            loss = ["loss", f"{loss_percent or 0}%"]
        return ["netem", "limit", str(self.queue_packets), "delay", *delay, *loss, "rate", f"{self.rate_kbit}kbit"]

    def up(self):
        self.down()
        sudo("ip", "netns", "add", self.ns)
        sudo("ip", "link", "add", self.host_if, "type", "veth", "peer", "name", self.sub_if, "netns", self.ns)
        sudo("ip", "addr", "add", f"{self.host_ip}/24", "dev", self.host_if)
        sudo("ip", "link", "set", self.host_if, "up")
        sudo("ip", "netns", "exec", self.ns, "ip", "addr", "add", f"{self.sub_ip}/24", "dev", self.sub_if)
        sudo("ip", "netns", "exec", self.ns, "ip", "link", "set", self.sub_if, "up")
        sudo("ip", "netns", "exec", self.ns, "ip", "link", "set", "lo", "up")
        # Generic segmentation would hand netem 64 kB packets; the link carries real ones.
        sudo("ethtool", "-K", self.host_if, "gso", "off", "tso", "off", "gro", "off", check=False)
        sudo("tc", "qdisc", "add", "dev", self.host_if, "root", *self.netem_args())

    def _netem(self, loss_percent):
        sudo("tc", "qdisc", "change", "dev", self.host_if, "root", *self.netem_args(loss_percent))

    def block(self):
        self._netem(100)
        self.blocked = True
        self.actions.append(dict(action="block", t=time.monotonic()))

    def unblock(self):
        self._netem(None)
        self.blocked = False
        self.actions.append(dict(action="unblock", t=time.monotonic()))

    def down(self):
        sudo("ip", "link", "del", self.host_if, check=False)
        sudo("ip", "netns", "del", self.ns, check=False)

    def command(self, argv):
        return ["sudo", "-n", "ip", "netns", "exec", self.ns, "sudo", "-n", "-u", getpass.getuser(), *argv]


class Neighbour:
    """A model device in a namespace of its own; its uplink enters the queue of the first hop."""

    def __init__(self, index, ifb):
        self.ns, self.host_if, self.dev_if = f"moqpub{NS_TAG}{index}", f"mqph{NS_TAG}{index}", f"mqpd{NS_TAG}{index}"
        self.host_ip, self.dev_ip = f"{NB_NET}.{index}.1", f"{NB_NET}.{index}.2"
        self.ifb = ifb

    def up(self):
        self.down()
        sudo("ip", "netns", "add", self.ns)
        sudo("ip", "link", "add", self.host_if, "type", "veth", "peer", "name", self.dev_if, "netns", self.ns)
        sudo("ip", "addr", "add", f"{self.host_ip}/24", "dev", self.host_if)
        sudo("ip", "link", "set", self.host_if, "up")
        sudo("ip", "netns", "exec", self.ns, "ip", "addr", "add", f"{self.dev_ip}/24", "dev", self.dev_if)
        sudo("ip", "netns", "exec", self.ns, "ip", "link", "set", self.dev_if, "up")
        sudo("ip", "netns", "exec", self.ns, "ip", "link", "set", "lo", "up")
        for ifc in (self.host_if,):
            sudo("ethtool", "-K", ifc, "gso", "off", "tso", "off", "gro", "off", check=False)
        sudo("ip", "netns", "exec", self.ns, "ethtool", "-K", self.dev_if, "gso", "off", "tso", "off", check=False)
        self.shared = subprocess.run(["ip", "link", "show", self.ifb], capture_output=True).returncode == 0
        if self.shared:
            sudo("tc", "qdisc", "add", "dev", self.host_if, "clsact")
            sudo("tc", "filter", "add", "dev", self.host_if, "ingress", "matchall",
                 "action", "mirred", "egress", "redirect", "dev", self.ifb)

    def down(self):
        sudo("ip", "link", "del", self.host_if, check=False)
        sudo("ip", "netns", "del", self.ns, check=False)

    def command(self, argv):
        return ["sudo", "-n", "ip", "netns", "exec", self.ns, "sudo", "-n", "-u", getpass.getuser(), *argv]


def outages(seed, span=120.0, gap=20.0, lo=0.2, hi=2.0):
    """The outages of tools/outages.py: exponential gaps, uniform durations, from a seed."""
    rng = random.Random(seed)
    t, out = 5.0, []
    while True:
        t += rng.expovariate(1 / gap)
        d = round(rng.uniform(lo, hi), 2)
        if t + d > span:
            break
        out.append((round(t, 2), round(t + d, 2)))
        t += d
    return out


class FirstHop:
    """Turns the relay log into events.jsonl of the run, line by line."""

    def __init__(self, relay_log, out):
        self.relay_log, self.out = relay_log, out
        self.pos = 0
        self.subscribed = []
        self.objects = 0
        self.done = set()

    def write(self, record):
        with (self.out / "events.jsonl").open("a") as f:
            f.write(json.dumps(record) + "\n")

    def poll(self):
        if not self.relay_log.exists():
            return
        with self.relay_log.open() as f:
            f.seek(self.pos)
            while True:
                line = f.readline()
                if not line.endswith("\n"):
                    break
                self.pos = f.tell()
                r = json.loads(line)
                ev = r["ev"]
                if ev == "device_setup":
                    self.write(dict(t=r["t"], event="setup", hop="device"))
                elif ev == "device_subscribed":
                    self.subscribed.append(r["t"])
                    self.write(dict(t=r["t"], event="subscribed", alias=r["alias"], track=r["track"], hop="device"))
                elif ev == "up_object":
                    self.objects += 1
                    self.write(dict(t=r["t"], event="object", alias=r["alias"], group=r["group"], object=r["object"],
                                    bytes=r["bytes"], properties=r["properties"], hop="device"))
                elif ev == "device_publish_done":
                    self.done.add(r["track"])
                    self.write(dict(t=r["t"], event="publish_done", track=r["track"], hop="device"))

    @property
    def epoch(self):
        return max(self.subscribed[:2]) if len(self.subscribed) >= 2 else None


def finished(path):
    if not path.exists():
        return False
    # a line that is still being written ends without a newline
    return any(json.loads(x).get("event") == "finished" for x in path.read_text().split("\n")[:-1] if x.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=14568)
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=150)
    p.add_argument("--policy", default="alert", choices=["fifo", "prio", "prio-to", "alert"])
    p.add_argument("--budget-ms", type=int, default=100)
    p.add_argument("--image-timeout-ms", type=int, default=10000, help="the deadline D of the images")
    p.add_argument("--no-cache", action="store_true")
    p.add_argument("--cc", choices=["cubic", "bbr"], default="cubic", help="congestion control of the relay")
    p.add_argument("--fixed-window", type=int, default=0,
                   help="ablation of policy alert: a constant image window in bytes (0: derived from the budget)")
    p.add_argument("--offload", action="store_true", help="the relay also forwards the camera frames for a helper (offload/jpeg)")
    p.add_argument("--offload-priority", type=int, default=None, help="subscriber priority of offload/jpeg at the device (relay default 0: first; 192: after the evidence)")
    p.add_argument("--up-image-track", help="name of the image track at the device (video firmware: preview/jpeg)")
    p.add_argument("--rdrive-ms", type=int, default=0,
                   help="receiver-driven unsubscription of the image track at this queueing target (base reproduction)")
    p.add_argument("--timeout-all", action="store_true",
                   help="the image timeout applies to every track, results included (base reproduction)")
    p.add_argument("--window-bdp", action="store_true",
                   help="policy alert: the window also holds the bytes on the wire (rate x smallest round trip)")
    p.add_argument("--sub", action="append", default=[], help="NAME:LINK:TRACKS")
    p.add_argument("--long", action="append", default=[], help="NAME:START:DUR, seconds after the subscription")
    p.add_argument("--seed", type=int, default=1, help="outages of subscriber i are drawn from seed*100+i")
    p.add_argument("--relay-bin", default="moq_relay", help="relay binary in relay/ (moq_relay_roles: the role-aware relay)")
    p.add_argument("--roles", action="store_true",
                   help="role-aware relay on offload/jpeg: fresh frames to the helper, evidence frames to the operator")
    p.add_argument("--helper-max-age-ms", type=int, default=0, help="--roles: a hard age limit for the helper's frames (0: none)")
    p.add_argument("--helper-deadline-ms", type=int, default=2000,
                   help="--roles: the operator's deadline for a helper result, from capture")
    p.add_argument("--helper-service-ms", type=int, default=450, help="--roles: relay to helper, inference, and back")
    p.add_argument("--helper-skip-evidence", action="store_true", help="--roles: the helper does not get the evidence frames")
    p.add_argument("--helper-free-window", action="store_true", help="no mission window on the helper session (its frames go at the congestion controller's pace)")
    p.add_argument("--emu", type=int, default=0, help="model cameras (emu_camera.py) sharing the helper through the relay (--emu-up)")
    p.add_argument("--emu-period", type=float, default=1.0, help="seconds between two helper frames of a model camera")
    p.add_argument("--emu-port", type=int, default=14569, help="the relay's port for the model cameras")
    p.add_argument("--helper-dispatch", type=int, default=0, help="--roles: frames outstanding at the helper (0: delivered as they arrive)")
    p.add_argument("--helper-admit", action="store_true", help="--helper-dispatch: a frame goes to a helper only if it can end in time there")
    p.add_argument("--emu-peer", action="store_true", help="the model cameras are helpers too (circular compute: emu_camera.py --peer)")
    p.add_argument("--emu-phases", default="", help="--emu-peer: busy:S,idle:S,... of each model camera, offset by camera")
    p.add_argument("--emu-phase-step", type=float, default=0.0, help="--emu-phases: offset between two model cameras, seconds")
    p.add_argument("--operator-all", action="store_true", help="--roles: the operator gets every frame of the shared track")
    p.add_argument("--sub-evidence", choices=["push", "fetch"], default="push",
                   help="evidence for the 'all' subscribers: pushed on its track, or fetched by group when a result asks")
    p.add_argument("--sub-image-track", default="evidence/jpeg",
                   help="the evidence track of the subscribers (offload/jpeg: the shared frame track, AIOT_MOQT_SHARED_FRAMES)")
    p.add_argument("--trace-csv", type=Path, default=Path.home() / "data/aiot_quic_results/wifi_trace/trace.csv")
    p.add_argument("--trace-offset", type=float, default=0.0)
    p.add_argument("--sub-port", type=int, default=14600)
    p.add_argument("--span", type=float, default=120.0, help="seconds after the subscription that the outages of the links cover")
    p.add_argument("--sub-idle-timeout", type=float, default=5.0)
    p.add_argument("--sub-connect-timeout", type=float, default=0, help="longest handshake of a subscriber; 0: idle timeout")
    p.add_argument("--sub-reconnect", type=float, default=1.0, help="pause of a subscriber before it connects again")
    p.add_argument("--neighbours", type=int, default=0)
    p.add_argument("--neighbour-sub", action="append", default=[], help="NAME:TRACKS")
    p.add_argument("--neighbour-unacked", type=int, default=32 * 1024, help="image bytes in flight of a neighbour")
    p.add_argument("--neighbour-image-bytes", type=int, default=50000)
    p.add_argument("--neighbour-cert", default=str(BASE / "vendor/nbcert/cert.pem"))
    p.add_argument("--neighbour-key", default=str(BASE / "vendor/nbcert/key.pem"))
    p.add_argument("--neighbour-ifb", default="moqv6test", help="the IFB of the first hop (flow_shaper.py)")
    # options of receiver_draft21.py that the trial passes and that do not apply here
    a, ignored = p.parse_known_args()
    if not a.sub:
        p.error("at least one --sub")
    a.out.mkdir(parents=True, exist_ok=True, mode=0o700)
    subs = []
    for i, spec in enumerate(a.sub):
        name, link, tracks = spec.split(":")
        if link not in LINKS or tracks not in ("results", "all"):
            p.error(f"--sub {spec}")
        shaper = wan_sub.WanSub(name) if link == "wan" else SubLink(i + 1, **LINKS[link])
        subs.append(dict(name=name, link=link, tracks=tracks, index=i + 1, shaper=shaper))
    longs = {}
    for spec in a.long:
        name, start, dur = spec.split(":")
        longs.setdefault(name, []).append((float(start), float(start) + float(dur)))

    nsubs = dict(x.split(":") for x in a.neighbour_sub)
    neighbours = [Neighbour(k + 1, a.neighbour_ifb) for k in range(a.neighbours)]
    procs = []
    stop = {"now": False}
    signal.signal(signal.SIGTERM, lambda *_: stop.update(now=True))
    signal.signal(signal.SIGINT, lambda *_: stop.update(now=True))
    hop = FirstHop(a.out / "relay.jsonl", a.out)
    echo = None
    if any(s["link"] == "wan" for s in subs):
        wan_sub.sync(BASE)
        echo = wan_sub.ClockEcho()
        echo.start()
    try:
        for s in subs:
            s["shaper"].up()
        for nb in neighbours:
            nb.up()
        relay = [str(BASE / "relay" / a.relay_bin), "--up", f"{a.host}:{a.port}", "--down", f"0.0.0.0:{a.sub_port}",
                 "--cert", a.cert, "--key", a.key, "--policy", a.policy, "--budget-ms", str(a.budget_ms),
                 "--image-timeout-ms", str(a.image_timeout_ms), "--log", str(a.out / "relay.jsonl"),
                 "--seconds", str(a.seconds + 10), "--cc", a.cc, *(["--no-cache"] if a.no_cache else []),
                 *(["--fixed-window", str(a.fixed_window)] if a.fixed_window else []),
                 *(["--window-bdp"] if a.window_bdp else []), *(["--timeout-all"] if a.timeout_all else []), *(["--rdrive-ms", str(a.rdrive_ms)] if a.rdrive_ms else []), *(["--up-image-track", a.up_image_track] if a.up_image_track else []), *(["--offload", "--helper-events"] if a.offload else []),
                 *(["--roles", "--helper-deadline-ms", str(a.helper_deadline_ms), "--helper-service-ms", str(a.helper_service_ms)] if a.roles else []),
                 *(["--helper-max-age-ms", str(a.helper_max_age_ms)] if a.roles and a.helper_max_age_ms else []), *(["--operator-all"] if a.operator_all else []), *(["--helper-skip-evidence"] if a.helper_skip_evidence else []), *(["--helper-free-window"] if a.helper_free_window else []),
                 *(["--emu-up", f"127.0.0.1:{a.emu_port}"] if a.emu else []), *(["--helper-dispatch", str(a.helper_dispatch)] if a.helper_dispatch else []), *(["--helper-admit"] if a.helper_admit else []),
                 *(["--offload-priority", str(a.offload_priority)] if a.offload_priority is not None else [])]
        procs.append(subprocess.Popen(relay, stdout=(a.out / "relay.out").open("w"), stderr=subprocess.STDOUT))
        relay_args = ["--cert", a.neighbour_cert, "--key", a.neighbour_key, "--policy", a.policy, "--budget-ms", str(a.budget_ms),
                      "--image-timeout-ms", str(a.image_timeout_ms), "--seconds", str(a.seconds + 10),
                      *(["--no-cache"] if a.no_cache else [])]
        for k, nb in enumerate(neighbours, 1):
            procs.append(subprocess.Popen(
                [str(BASE / "relay/moq_relay"), "--up", f"{nb.host_ip}:{a.port + 10 + k}", "--down", f"0.0.0.0:{a.sub_port + k}",
                 "--log", str(a.out / f"relay-nb{k}.jsonl"), *relay_args],
                stdout=(a.out / f"relay-nb{k}.out").open("w"), stderr=subprocess.STDOUT))
        time.sleep(0.3)
        for k, nb in enumerate(neighbours, 1):
            argv = [str(PYTHON), "-u", str(BASE / "device_main.py"), "--policy", "D",
                    "--host", nb.host_ip, "--port", str(a.port + 10 + k), "--cert", a.neighbour_cert, "--out", str(a.out / f"nb{k}"),
                    "--seed", str(a.seed * 10 + k), "--produce", str(a.seconds + 30), "--drain", "5",
                    "--inference", "1.25", "--deadline", str(a.image_timeout_ms / 1000),
                    "--image-bytes", str(a.neighbour_image_bytes), "--events", "0,0,0,1",
                    "--idle-timeout", "5", "--unacked-image", str(a.neighbour_unacked)]
            procs.append(subprocess.Popen(nb.command(argv), stdout=(a.out / f"nb{k}.log").open("w"), stderr=subprocess.STDOUT))
        for s in subs:
            if s["name"] not in nsubs:
                continue
            for k in range(1, len(neighbours) + 1):
                argv = [str(PYTHON), "-u", str(BASE / "receiver_draft21.py"), "--connect", f"{s['shaper'].host_ip}:{a.sub_port + k}",
                        "--evidence", "push" if nsubs[s["name"]] == "all" else "none", "--out", str(a.out / f"nb{k}-{s['name']}"),
                        "--seconds", str(a.seconds), "--idle-timeout", str(a.sub_idle_timeout), "--authority", "relay"]
                procs.append(subprocess.Popen(s["shaper"].command(argv), stdout=(a.out / f"nb{k}-{s['name']}.log").open("w"),
                                              stderr=subprocess.STDOUT))
        for s in subs:
            out = a.out / f"sub-{s['name']}"
            args = ["--connect", f"{s['shaper'].host_ip}:{a.sub_port}",
                    "--evidence", a.sub_evidence if s["tracks"] == "all" else "none", "--image-track", a.sub_image_track,
                    "--seconds", str(a.seconds), "--idle-timeout", str(a.sub_idle_timeout), "--authority", "relay",
                    "--connect-timeout", str(a.sub_connect_timeout), "--reconnect", str(a.sub_reconnect)]
            if a.offload:
                args.append("--helper-events")
            if s["link"] == "wan":
                s["proc"] = s["shaper"].start(args, out)
            else:
                argv = [str(PYTHON), "-u", str(BASE / "receiver_draft21.py"), *args, "--out", str(out)]
                s["proc"] = subprocess.Popen(s["shaper"].command(argv), stdout=(a.out / f"sub-{s['name']}.log").open("w"),
                                             stderr=subprocess.STDOUT)
            procs.append(s["proc"])
        print(json.dumps(dict(t=time.monotonic(), event="relay_started", policy=a.policy, ignored=ignored,
                              subs=[{k: v for k, v in s.items() if k in ("name", "link", "tracks")} for s in subs])), flush=True)
        end = time.monotonic() + a.seconds
        schedules = None
        emu_started = False
        while time.monotonic() < end and not stop["now"]:
            hop.poll()
            if a.emu and hop.epoch is not None and not emu_started:
                # the model cameras start with the run of the real camera (its subscription), for the span of the run
                emu_started = True
                for k in range(1, a.emu + 1):
                    argv = [str(PYTHON), "-u", str(BASE / "emu_camera.py"), "--up", f"127.0.0.1:{a.emu_port}", "--dev", str(k),
                            "--out", str(a.out / "emu"), "--period", str(a.emu_period), "--seconds", str(a.span),
                            "--delay", str(a.emu_period * (k - 1) / max(a.emu, 1)), "--skip", str(400 * k),
                            *(["--peer", f"127.0.0.1:{a.sub_port}"] if a.emu_peer else []),
                            *(["--phases", a.emu_phases, "--phase-offset", str(a.emu_phase_step * (k - 1))] if a.emu_peer and a.emu_phases else [])]
                    procs.append(subprocess.Popen(argv, stdout=(a.out / f"emu{k}.log").open("w"), stderr=subprocess.STDOUT))
                print(json.dumps(dict(t=time.monotonic(), event="emu_started", cameras=a.emu, period=a.emu_period)), flush=True)
            if hop.epoch is not None and schedules is None:
                schedules = []
                for s in subs:
                    cfg = LINKS[s["link"]]
                    # a subscriber with a long outage has that one only, so that no two overlap
                    spans = longs.get(s["name"]) or (outages(a.seed * 100 + s["index"], span=a.span) if cfg["outages"] else [])
                    spans = sorted(spans)
                    if spans:
                        schedules.append(OutageSchedule(s["shaper"], spans))
                    if s["link"] == "trace":
                        schedules.append(DelayTrace(s["shaper"], load_delay_trace(a.trace_csv, a.trace_offset, a.span + 10)))
                    s["spans"] = spans
            for sch in schedules or []:
                sch.step(time.monotonic() - hop.epoch)
            if subs and all(finished(a.out / f"sub-{s['name']}" / "events.jsonl") for s in subs):
                hop.poll()
                hop.write(dict(t=time.monotonic(), event="finished", objects=hop.objects,
                               subscribers={s["name"]: s["link"] for s in subs}, hop="device"))
                break
            if any(s["proc"].poll() is not None and not finished(a.out / f"sub-{s['name']}" / "events.jsonl") for s in subs):
                print(json.dumps(dict(t=time.monotonic(), event="error", reason="a subscriber exited")), flush=True)
            time.sleep(0.05)
        for sch in schedules or []:
            sch.finish()
        (a.out / "links.json").write_text(json.dumps(dict(
            epoch=hop.epoch, policy=a.policy, budget_ms=a.budget_ms, seed=a.seed, cc=a.cc, fixed_window=a.fixed_window, window_bdp=a.window_bdp, timeout_all=a.timeout_all, rdrive_ms=a.rdrive_ms,
            neighbours=dict(n=a.neighbours, subs=nsubs, unacked=a.neighbour_unacked, image_bytes=a.neighbour_image_bytes),
            subscribers=[dict(name=s["name"], link=s["link"], tracks=s["tracks"], netem=LINKS[s["link"]],
                              outages=s.get("spans", []), actions=s["shaper"].actions) for s in subs]), indent=1) + "\n")
        # keep running until the trial stops us, so that its last connection closes cleanly
        while not stop["now"] and time.monotonic() < end + 5:
            hop.poll()
            time.sleep(0.2)
    finally:
        for proc in procs:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for proc in procs:
            try:
                proc.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                proc.kill()
        hop.poll()
        for s in subs:
            s["shaper"].down()
        if echo:
            echo.stop()
        for nb in neighbours:
            nb.down()


if __name__ == "__main__":
    sys.exit(main())
