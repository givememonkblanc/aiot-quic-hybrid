#!/usr/bin/env python3
"""TCP-MQTT baseline logging subscriber.

Subscribes to the benchmark topics, parses the same aiot_object_header the
device prepends, and writes rows in the *exact* dgram_recv.csv schema the QUIC
receiver emits — so parse_receiver_dgram_log.py computes delivery/loss/OWDV
identically and TCP-vs-QUIC is directly comparable.

aiot_object_header_t (packed, little-endian, 12 B):
  uint8 magic_ver, uint8 track_id, uint8 priority, uint8 flags,
  uint32 object_seq, uint32 gen_ts_ms
"""
import argparse
import struct
import ssl
import sys
import time

import paho.mqtt.client as mqtt

HDR = struct.Struct("<BBBBII")          # 12 bytes
HDR_LEN = HDR.size
COLS = "recv_us,cid,valid,track_id,priority,flags,object_seq,gen_ts_ms,payload_len\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--topic", default="aiot/bench/#")
    ap.add_argument("--out", default="tcp_dgram.csv")
    ap.add_argument("--seconds", type=int, default=0, help="0 = run until Ctrl-C")
    ap.add_argument("--tls", action="store_true", default=True)
    ap.add_argument("--no-tls", dest="tls", action="store_false")
    args = ap.parse_args()

    out = open(args.out, "w")
    out.write(COLS)
    state = {"n": 0}

    def on_connect(client, userdata, flags, rc, *a):
        client.subscribe(args.topic)
        sys.stderr.write(f"subscribed {args.topic} (rc={rc})\n")

    def on_message(client, userdata, msg):
        p = msg.payload
        if len(p) < HDR_LEN:
            return
        magic, track, prio, flags, seq, gen_ts = HDR.unpack_from(p, 0)
        recv_us = int(time.time() * 1_000_000)
        out.write(f"{recv_us},tcp,1,{track},{prio},{flags},{seq},{gen_ts},{len(p) - HDR_LEN}\n")
        state["n"] += 1

    client = mqtt.Client()
    if args.tls:
        # Encrypted but not authenticating the broker — matches the device's
        # testbed esp-tls config (skip_common_name). Note in the paper.
        client.tls_set(cert_reqs=ssl.CERT_NONE)
        client.tls_insecure_set(True)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=60)

    try:
        if args.seconds > 0:
            client.loop_start()
            time.sleep(args.seconds)
            client.loop_stop()
        else:
            client.loop_forever()
    except KeyboardInterrupt:
        pass
    finally:
        out.flush()
        out.close()
        sys.stderr.write(f"logged {state['n']} objects -> {args.out}\n")


if __name__ == "__main__":
    main()
