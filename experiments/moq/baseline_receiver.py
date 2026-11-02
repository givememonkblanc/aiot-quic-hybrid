#!/usr/bin/env python3
"""Receiver of a baseline run, in place of relay_receiver.py in remote_ota_trial.py: the device
publishes to a server that is not the MOQT relay of this work, and the subscribers receive from it
over the same emulated links as in the relay runs (relay_receiver.LINKS, the same outages per seed).

  --server mqtt     a mosquitto broker; the device runs the MQTT baseline (moq_mqtt_shim.c) and
                    connects with TLS over TCP to port 14568; the subscribers are mqtt_subscriber.py
  --server imquic   the MOQT relay of Meetecho imquic (draft-21, raw QUIC) on UDP 14568; the device
                    and the subscribers (receiver_draft21.py) are those of the relay runs

A tap subscribes on the host without a link. Its arrivals stand for the arrivals at the server (the
first hop) and are written to relay.jsonl in the format of the relay log, with the result track as
alias 2 and the image track as alias 3, so that analyze_relay.py reads these runs unchanged. The
second subscription of the tap is the epoch of the run, as the second subscription of the relay is
in a relay run. The run ends when every subscriber has finished."""
import argparse
import json
import signal
import subprocess
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from flow_shaper import DelayTrace, OutageSchedule, load_delay_trace
from relay_receiver import LINKS, PYTHON, SubLink, finished, outages
import wan_sub

BASE = Path(__file__).resolve().parent
IMQUIC = BASE / "vendor/imquic"
CA = BASE / "vendor/nbcert/cert.pem"
KEY = BASE / "vendor/nbcert/key.pem"
TRACK_ALIAS = {"ai/events": 2, "evidence/jpeg": 3}


class Tap:
    """The events of the tap subscriber as the first hop: relay.jsonl and events.jsonl of the run."""

    def __init__(self, tap_events, out):
        self.src, self.out = tap_events, out
        self.pos = 0
        self.alias = {}
        self.subscribed = []
        self.objects = 0

    def _append(self, name, record):
        with (self.out / name).open("a") as f:
            f.write(json.dumps(record) + "\n")

    def poll(self):
        if not self.src.exists():
            return
        with self.src.open() as f:
            f.seek(self.pos)
            while True:
                line = f.readline()
                if not line.endswith("\n"):
                    break
                self.pos = f.tell()
                r = json.loads(line)
                ev, t = r["event"], r["t"]
                if ev == "setup":
                    self._append("relay.jsonl", dict(t=t, ev="device_setup"))
                    self._append("events.jsonl", dict(t=t, event="setup", hop="device"))
                elif ev == "subscribed":
                    alias = TRACK_ALIAS.get(r.get("track"), r.get("alias"))
                    self.alias[r.get("alias")] = alias
                    if len(self.subscribed) < 2:
                        self.subscribed.append(t)
                        self._append("relay.jsonl", dict(t=t, ev="device_subscribed", alias=alias, track=r.get("track")))
                        self._append("events.jsonl", dict(t=t, event="subscribed", alias=alias, track=r.get("track"), hop="device"))
                elif ev in ("object", "fetch_object"):
                    alias = self.alias.get(r.get("alias"), r.get("alias"))
                    self.objects += 1
                    rec = dict(t=t, alias=alias, group=r["group"], object=r.get("object", 0), bytes=r["bytes"],
                               properties=r.get("properties", {}))
                    self._append("relay.jsonl", dict(ev="up_object", **rec))
                    self._append("events.jsonl", dict(event="object", hop="device", **rec))
                elif ev == "publish_done":
                    self._append("relay.jsonl", dict(t=t, ev="device_publish_done", track=r.get("track")))
                    self._append("events.jsonl", dict(t=t, event="publish_done", track=r.get("track"), hop="device"))

    @property
    def epoch(self):
        return max(self.subscribed[:2]) if len(self.subscribed) >= 2 else None


def normalise_aliases(path):
    """Subscriber events of imquic carry the aliases of that relay; the analysis expects 2 and 3."""
    if not path.exists():
        return
    rows = [json.loads(x) for x in path.read_text().splitlines() if x.strip()]
    alias = {}
    for r in rows:
        if r["event"] == "subscribed" and r.get("track") in TRACK_ALIAS:
            alias[r["alias"]] = TRACK_ALIAS[r["track"]]
            r["server_alias"], r["alias"] = r["alias"], TRACK_ALIAS[r["track"]]
        elif r["event"] in ("object", "fetch_object", "end_of_group", "reset") and r.get("alias") in alias:
            r["server_alias"], r["alias"] = r["alias"], alias[r["alias"]]
    path.write_text("".join(json.dumps(r) + "\n" for r in rows))


def mosquitto_conf(out, port, sub_port, tap_port, cert, key):
    conf = out / "mosquitto.conf"
    conf.write_text("\n".join([
        "per_listener_settings false", "allow_anonymous true", "persistence false",
        f"log_dest file {out / 'mosquitto.log'}", "log_type error", "log_type warning", "log_type notice",
        "log_type information", "connection_messages true", "log_timestamp_format %Y-%m-%dT%H:%M:%S",
        f"listener {port} 0.0.0.0", f"certfile {cert}", f"keyfile {key}",
        f"listener {sub_port} 0.0.0.0", f"certfile {cert}", f"keyfile {key}",
        f"listener {tap_port} 127.0.0.1", ""]))
    return conf


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--server", choices=["mqtt", "imquic"], required=True)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=14568)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=150)
    p.add_argument("--sub", action="append", default=[], help="NAME:LINK:TRACKS")
    p.add_argument("--long", action="append", default=[], help="NAME:START:DUR, seconds after the subscription")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--trace-csv", type=Path, default=Path.home() / "data/aiot_quic_results/wifi_trace/trace.csv")
    p.add_argument("--trace-offset", type=float, default=0.0)
    p.add_argument("--sub-port", type=int, default=14600)
    p.add_argument("--span", type=float, default=120.0, help="seconds after the subscription that the outages of the links cover")
    p.add_argument("--tap-port", type=int, default=14599)
    p.add_argument("--sub-idle-timeout", type=float, default=5.0)
    p.add_argument("--sub-reconnect", type=float, default=1.0)
    p.add_argument("--cert", default=str(CA), help="imquic: the certificate that the device has pinned")
    p.add_argument("--key", default=str(KEY))
    p.add_argument("--start-wait", type=float, default=30.0, help="MQTT: longest wait for the subscribers before the start")
    # the trial passes --cert/--key of the receivers; imquic presents them, mosquitto vendor/nbcert, which
    # the MQTT image carries
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
    procs = []
    stop = {"now": False}
    signal.signal(signal.SIGTERM, lambda *_: stop.update(now=True))
    signal.signal(signal.SIGINT, lambda *_: stop.update(now=True))
    tap = Tap(a.out / "tap" / "events.jsonl", a.out)
    starter = None
    echo = None
    if any(s["link"] == "wan" for s in subs):
        wan_sub.sync(BASE)
        echo = wan_sub.ClockEcho()
        echo.start()
    try:
        for s in subs:
            s["shaper"].up()
        if a.server == "mqtt":
            conf = mosquitto_conf(a.out, a.port, a.sub_port, a.tap_port, CA, KEY)
            procs.append(subprocess.Popen(["/usr/sbin/mosquitto", "-c", str(conf)],
                                          stdout=(a.out / "server.out").open("w"), stderr=subprocess.STDOUT))
            time.sleep(0.5)
            tap_argv = [str(PYTHON), "-u", str(BASE / "mqtt_subscriber.py"), "--connect", f"127.0.0.1:{a.tap_port}",
                        "--client-id", "tap", "--tracks", "all", "--tap", "--out", str(a.out / "tap"),
                        "--seconds", str(a.seconds)]
        else:
            env = {"LD_LIBRARY_PATH": f"{IMQUIC / 'prefix/lib'}:{IMQUIC / 'imquic/src/.libs'}"}
            procs.append(subprocess.Popen(
                # one port for the device and the subscribers, so on every interface (the trial passes
                # the address of the device side as --host)
                [str(IMQUIC / "imquic/examples/.libs/imquic-moq-relay"), "-b", "0.0.0.0", "-p", str(a.port),
                 "-c", a.cert, "-k", a.key, "-q", "-M", "21", "-Z", "-T", "-d", "4"],
                env=env, stdout=(a.out / "server.out").open("w"), stderr=subprocess.STDOUT))
            time.sleep(0.5)
            tap_argv = [str(PYTHON), "-u", str(BASE / "receiver_draft21.py"), "--connect", f"127.0.0.1:{a.port}",
                        "--evidence", "push", "--out", str(a.out / "tap"), "--seconds", str(a.seconds),
                        "--idle-timeout", str(a.sub_idle_timeout), "--authority", "relay", "--reconnect", "0.5",
                        "--lenient-fetch-end"]
        procs.append(subprocess.Popen(tap_argv, stdout=(a.out / "tap.log").open("w"), stderr=subprocess.STDOUT))
        for s in subs:
            out = a.out / f"sub-{s['name']}"
            wan = s["link"] == "wan"
            if a.server == "mqtt":
                program = "mqtt_subscriber.py"
                args = ["--connect", f"{s['shaper'].host_ip}:{a.sub_port}",
                        "--ca", wan_sub.REMOTE_CA if wan else str(CA), "--client-id", f"sub-{s['name']}", "--tracks", s["tracks"],
                        "--seconds", str(a.seconds), "--keepalive", str(int(a.sub_idle_timeout)),
                        "--reconnect", str(a.sub_reconnect)]
            else:
                program = "receiver_draft21.py"
                args = ["--connect", f"{s['shaper'].host_ip}:{a.port}",
                        "--evidence", "push" if s["tracks"] == "all" else "none",
                        "--seconds", str(a.seconds), "--idle-timeout", str(a.sub_idle_timeout), "--authority", "relay",
                        "--reconnect", str(a.sub_reconnect), "--lenient-fetch-end"]
            if wan:
                s["proc"] = s["shaper"].start(args, out, program)
            else:
                argv = [str(PYTHON), "-u", str(BASE / program), *args, "--out", str(out)]
                s["proc"] = subprocess.Popen(s["shaper"].command(argv), stdout=(a.out / f"sub-{s['name']}.log").open("w"),
                                             stderr=subprocess.STDOUT)
            procs.append(s["proc"])
        print(json.dumps(dict(t=time.monotonic(), event="baseline_started", server=a.server, ignored=ignored,
                              subs=[{k: v for k, v in s.items() if k in ("name", "link", "tracks")} for s in subs])), flush=True)
        if a.server == "mqtt":
            # The device starts when it is told to, once every subscriber holds its session, as the
            # device of a relay run starts when the relay subscribes to it.
            ready_by = time.monotonic() + a.start_wait
            def connected(name):
                ev = a.out / f"sub-{name}" / "events.jsonl"
                return ev.exists() and '"setup"' in ev.read_text()
            while time.monotonic() < ready_by and not stop["now"] and not all(connected(s["name"]) for s in subs):
                time.sleep(0.1)
            time.sleep(1.0)    # their subscriptions follow the connection
            starter = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="starter")
            starter.connect("127.0.0.1", a.tap_port)
            starter.loop_start()
            starter.publish("aiot/host/start", b"1", qos=1, retain=True).wait_for_publish(5)
            print(json.dumps(dict(t=time.monotonic(), event="start_published")), flush=True)
        end = time.monotonic() + a.seconds
        schedules = None
        while time.monotonic() < end and not stop["now"]:
            tap.poll()
            if tap.epoch is not None and schedules is None:
                schedules = []
                for s in subs:
                    cfg = LINKS[s["link"]]
                    spans = sorted(longs.get(s["name"]) or (outages(a.seed * 100 + s["index"], span=a.span) if cfg["outages"] else []))
                    if spans:
                        schedules.append(OutageSchedule(s["shaper"], spans))
                    if s["link"] == "trace":
                        schedules.append(DelayTrace(s["shaper"], load_delay_trace(a.trace_csv, a.trace_offset, a.span + 10)))
                    s["spans"] = spans
            for sch in schedules or []:
                sch.step(time.monotonic() - tap.epoch)
            if all(finished(a.out / f"sub-{s['name']}" / "events.jsonl") for s in subs):
                tap.poll()
                tap._append("events.jsonl", dict(t=time.monotonic(), event="finished", objects=tap.objects,
                                                  subscribers={s["name"]: s["link"] for s in subs}, hop="device"))
                break
            time.sleep(0.05)
        for sch in schedules or []:
            sch.finish()
        (a.out / "links.json").write_text(json.dumps(dict(
            epoch=tap.epoch, policy=a.server, budget_ms=100, seed=a.seed, server=a.server,
            subscribers=[dict(name=s["name"], link=s["link"], tracks=s["tracks"], netem=LINKS[s["link"]],
                              outages=s.get("spans", []), actions=s["shaper"].actions) for s in subs]), indent=1) + "\n")
        while not stop["now"] and time.monotonic() < end + 5:
            tap.poll()
            time.sleep(0.2)
    finally:
        if starter is not None:
            starter.loop_stop()
        for proc in procs:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for proc in procs:
            try:
                proc.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                proc.kill()
        tap.poll()
        if a.server == "imquic":
            for s in subs:
                normalise_aliases(a.out / f"sub-{s['name']}" / "events.jsonl")
        for s in subs:
            s["shaper"].down()
        if echo:
            echo.stop()


if __name__ == "__main__":
    sys.exit(main())
