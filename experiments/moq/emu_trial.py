#!/usr/bin/env python3
"""One emulated run: remote_ota_trial.py with the model device of emu_device.py in place of the
ESP32 (Orin; run as root, as the tools of the device runs call `sudo -n`).

The model device runs in the network namespace emudev and reaches the receiver (relay_receiver.py
or baseline_receiver.py, with its subscribers) over the veth pair emuv0/emuv1. The first hop is
shaped as in the device runs: the device flow to port 14568 entering emuv0 is redirected to the
IFB emuifb, where netem applies rate, delay, jitter, loss and the outages (flow_shaper.py). The
run directory has the files of a device run (manifest.json, serial.log, events.jsonl, links.json,
relay.jsonl, sub-*), so analyze_matrix.py and analyze_oracle.py apply unchanged.

usage: emu_trial.py --name N --emu mbm-floor --trace serial.log [--scale k --cluster n --fp p]
       --shaped --rate-kbit 128 [--delay-ms 5 --jitter-ms 5 --ge-loss 1:30:50:0 --outage 40:10]
       --receiver-arg=--policy --receiver-arg=alert ..."""
import argparse
import json
import os
import subprocess
import time
from pathlib import Path

from flow_shaper import FlowShaper, OutageSchedule, parse_outages

BASE = Path(__file__).resolve().parent
PYTHON = BASE / "vendor/venv/bin/python"
NS, HOST_IF, DEV_IF, HOST_IP, DEV_IP = "emudev", "emuv0", "emuv1", "10.77.0.1", "10.77.0.2"
CERT, KEY = BASE / "vendor/nbcert/cert.pem", BASE / "vendor/nbcert/key.pem"


ROOT_CMD = [] if os.geteuid() == 0 else ["sudo", "-n"]
# the receiver and its subscriber links carry this tag (relay_receiver.py AIOT_NS_TAG), and the
# relay serves the subscribers on another port, so that a device run on the same host is not met
NS_TAG, SUB_PORT = "e", 15600


def sh(*args, check=True):
    return subprocess.run(ROOT_CMD + list(args), check=check, text=True, capture_output=True)


class EmuShaper(FlowShaper):
    """FlowShaper on the veth of the model device."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.device, self.ifb, self.peer = HOST_IF, "emuifb", DEV_IP

    def on(self):
        if sh("ip", "link", "show", self.ifb, check=False).returncode == 0:
            raise RuntimeError("emuifb exists: another emulated run")
        try:
            sh("ip", "link", "add", self.ifb, "type", "ifb")
            self.created = True
            sh("ip", "link", "set", self.ifb, "up")
            sh("tc", "qdisc", "add", "dev", self.ifb, "root", *self.netem_args())
            sh("tc", "qdisc", "add", "dev", self.device, "clsact")
            self.clsact = True
            sh("tc", "filter", "add", "dev", self.device, "ingress", "protocol", "ip", "pref", "42426", "flower",
               "src_ip", DEV_IP, "dst_ip", HOST_IP, "ip_proto", self.ip_proto, "dst_port", "14568",
               "action", "mirred", "egress", "redirect", "dev", self.ifb)
            self.filter = True
        except BaseException:
            self.off()
            raise


def netns_up():
    sh("ip", "netns", "del", NS, check=False)
    sh("ip", "link", "del", HOST_IF, check=False)
    sh("ip", "netns", "add", NS)
    sh("ip", "link", "add", HOST_IF, "type", "veth", "peer", "name", DEV_IF, "netns", NS)
    sh("ip", "addr", "add", f"{HOST_IP}/30", "dev", HOST_IF)
    sh("ip", "link", "set", HOST_IF, "up")
    for args in (("ip", "addr", "add", f"{DEV_IP}/30", "dev", DEV_IF), ("ip", "link", "set", DEV_IF, "up"),
                 ("ip", "link", "set", "lo", "up")):
        sh("ip", "netns", "exec", NS, *args)


def netns_down():
    sh("ip", "link", "del", HOST_IF, check=False)
    sh("ip", "netns", "del", NS, check=False)


def has_finished(path):
    if not path.exists():
        return False
    for line in path.read_text().splitlines():
        try:
            if json.loads(line).get("event") == "finished":
                return True
        except json.JSONDecodeError:
            pass
    return False


def subscription_epoch(path):
    if not path.exists():
        return None
    starts = []
    for line in path.read_text().splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if row.get("event") == "subscribed":
            starts.append(row["t"])
    return max(starts[:2]) if len(starts) >= 2 else None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", required=True)
    p.add_argument("--emu", required=True)
    p.add_argument("--trace", type=Path, required=True)
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--cluster", type=int, default=1)
    p.add_argument("--fp", type=float, default=0.0)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--seconds", type=float, default=213)
    p.add_argument("--receiver", default="relay_receiver.py")
    p.add_argument("--receiver-arg", action="append", default=[])
    p.add_argument("--shaped", action="store_true")
    p.add_argument("--rate-kbit", type=int, default=512)
    p.add_argument("--delay-ms", type=int, default=5)
    p.add_argument("--jitter-ms", type=float, default=0)
    p.add_argument("--loss-percent", type=float, default=0)
    p.add_argument("--ge-loss")
    p.add_argument("--queue-packets", type=int, default=64)
    p.add_argument("--outage", action="append", default=[])
    p.add_argument("--timeout", type=float, default=440)
    p.add_argument("--results", type=Path, default=BASE / "results")
    a = p.parse_args()
    ge = [float(x) for x in a.ge_loss.split(":")] if a.ge_loss else None
    outages = parse_outages(a.outage)
    out = a.results / a.name
    out.mkdir(parents=True, exist_ok=False)
    manifest = dict(name=a.name, emulated=True, emu=a.emu, trace=str(a.trace), scale=a.scale, cluster=a.cluster,
                    fp=a.fp, seed=a.seed, status="running", clock_offset_s=time.time() - time.monotonic(),
                    network=(f"{a.rate_kbit}kbit/s device-to-host, {a.delay_ms}ms netem" if a.shaped else "unshaped"),
                    impairment=dict(rate_kbit=a.rate_kbit, delay_ms=a.delay_ms, jitter_ms=a.jitter_ms, ge_loss=ge,
                                    loss_percent=a.loss_percent, queue_packets=a.queue_packets),
                    outages=[dict(start_s=s, end_s=e) for s, e in outages], outage_actions=[],
                    test_release=dict(elf_sha256=f"emu-{a.emu}"))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    procs, shaper, schedule = [], None, None
    try:
        netns_up()
        if a.shaped:
            shaper = EmuShaper(a.rate_kbit, a.delay_ms, a.queue_packets, a.loss_percent, a.jitter_ms, ge)
            shaper.on()
            (out / "shaper-start.txt").write_text(shaper.stats())
        receiver = subprocess.Popen(
            [str(PYTHON), "-u", str(BASE / a.receiver), *a.receiver_arg, "--host", HOST_IP, "--port", "14568",
             "--sub-port", str(SUB_PORT), "--cert", str(CERT), "--key", str(KEY), "--out", str(out),
             "--seconds", str(a.timeout + 30)],
            cwd=BASE, stdout=(out / "receiver.log").open("w"), stderr=subprocess.STDOUT,
            env=dict(os.environ, AIOT_NS_TAG=NS_TAG))
        procs.append(receiver)
        time.sleep(2)
        if receiver.poll() is not None:
            raise RuntimeError("receiver failed to start")
        device = subprocess.Popen(
            [*ROOT_CMD, "ip", "netns", "exec", NS, *(ROOT_CMD + ["-u", os.environ.get("USER", "root")] if ROOT_CMD else []),
             str(PYTHON), "-u", str(BASE / "emu_device.py"), "--emu", a.emu,
             "--trace", str(a.trace), "--host", HOST_IP, "--cert", str(CERT), "--serial", str(out / "serial.log"),
             "--seconds", str(a.seconds), "--scale", str(a.scale), "--cluster", str(a.cluster), "--fp", str(a.fp),
             "--seed", str(a.seed)],
            cwd=BASE, stdout=(out / "device.log").open("w"), stderr=subprocess.STDOUT)
        procs.append(device)
        schedule = OutageSchedule(shaper, outages) if outages else None
        deadline = time.monotonic() + a.timeout
        while time.monotonic() < deadline:
            if receiver.poll() is not None:
                raise RuntimeError("receiver exited before completion")
            if has_finished(out / "events.jsonl"):
                break
            epoch = subscription_epoch(out / "events.jsonl")
            wait = 1.0
            if schedule is not None and epoch is not None:
                before = len(schedule.applied)
                manifest["outage_actions"] = schedule.step(time.monotonic() - epoch)
                if len(schedule.applied) != before:
                    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
                due = schedule.next_in(time.monotonic() - epoch)
                if due is not None:
                    wait = min(1.0, max(0.05, due))
            time.sleep(wait)
        if not has_finished(out / "events.jsonl"):
            raise RuntimeError("completion timeout")
        time.sleep(3)
        manifest.update(status="complete")
    except Exception as error:
        manifest.update(status="failed", reason=str(error))
    finally:
        if schedule is not None:
            schedule.finish()
            manifest["outage_actions"] = schedule.applied
        for proc in reversed(procs):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
        if shaper is not None:
            try:
                (out / "shaper-end.txt").write_text(shaper.stats())
            except Exception:
                pass
            shaper.off()
        netns_down()
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(manifest["status"], manifest.get("reason", ""))


if __name__ == "__main__":
    main()
