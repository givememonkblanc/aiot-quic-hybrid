# AIoT Presence — Backend & Web App

A small product web app that turns the receiver's raw MQTT-over-QUIC traffic
into a live occupancy dashboard behind a login, with history and alerts.

```
                                                       ┌─▶ dashboard.py  (관리: 로그인·대시보드·온보딩·설정)
device (ESP32-S3) ──QUIC──▶ picoquic receiver ──feed──┤
   MQTT-over-QUIC            (+ SQLite aiot.db)         └─▶ spaceapp.py   (응용: 공간 현황 & 이용률)
```

Two services share one data layer (the feed + `aiot.db`):

- **`dashboard.py`** — the management console: sign in, live occupancy, add
  devices, settings.
- **`spaceapp.py`** — a separate application built on the same sensors: find a
  free room now, and see per-space utilization (today's occupancy %, visits,
  hourly heatmap). Read-only; run it alongside the dashboard.

`./backend/run.sh 8080` launches the receiver + dashboard (`:8080`) + spaces
(`:8090`, i.e. dashboard port + 10) together.

## Run

```bash
# needs host_receiver/picoquic/picoquicdemo already built
AIOT_PASS=your-password ./backend/run.sh 8080
# open http://localhost:8080/  → sign in as  admin / your-password
```

`run.sh` starts the picoquic receiver (writing `aiot_feed.jsonl`) and the web
app together; Ctrl-C stops both. To run the app alone against an existing feed:

```bash
AIOT_PASS=secret python3 backend/dashboard.py --port 8080
```

First launch sets the `admin` password from `AIOT_PASS` (default `admin` —
change it under **Settings**). It is stored hashed (PBKDF2) in SQLite, so later
launches keep whatever you set.

## Pages

For the ESP32 handoff research firmware, run the dashboard with `--research`
and `--feed PATH/TO/feed.jsonl`. This displays typed camera/AI status,
synthetic telemetry, critical events, AI results, and JPEG chunks without
device registration. `--image PATH/TO/latest.jpg` enables the camera preview.
Counts cover only the last 256 KiB of the feed, not total delivery rates;
file modification age indicates receiver activity. The API retains up to 200
recent messages from that window; the page filters them by type and displays
the newest 20 with expandable JSON details. AI results are joined to
`ai_recv.csv` next to the feed (override with `--ai-log`) by connection ID,
object sequence, generation timestamp, and receiver timestamp. Binary payloads
in the text feed are not used to reconstruct AI values. JPEG rows represent
chunks, not confirmed complete frames. The page marks the feed offline after
20 seconds without updates. Board generation timestamps are not interpreted
as wall-clock times or one-way latency; AI raw scores are not accuracy.

The fixed status panel beside the images retains the latest camera frame,
AI result, telemetry, memory report, and critical event independently. A
background reader bootstraps from the last 4 MiB and then consumes new complete
lines, so camera traffic does not evict infrequent status updates. Receiver
timestamps use the local Linux monotonic clock; ages describe time since
receipt, not device-to-server latency. JPEG completion requires contiguous
chunk sequences and SOI/EOI markers (not an image-quality validation).
AI/frame arrival differences match connection ID and generation timestamp;
positive differences mean the AI result arrived after frame assembly.
The latest snapshot age uses the image processor's publication time instead.
Periodic rows warn after three times their recent median receive interval,
with a 3-second minimum or a 20-second fallback before interval samples exist.
Event-driven rows do not warn just because no new event occurred.

- **`/login`** — session sign-in (HttpOnly cookie). Everything else requires it.
- **`/`** — live occupancy: a **live camera view** (MJPEG), OCCUPIED / VACANT
  per device, time-in-state, a presence-history strip, today's occupied time &
  visit count, device health (motion score, RSSI, heap), and an event timeline.
  Toggle **🔔 알림** for a browser notification whenever someone enters.
- **Live video** — the firmware streams JPEG frames on the QUIC image track;
  the receiver reassembles them to `latest.jpg` (`AIOT_IMAGE_OUT`) and the app
  serves `GET /stream` (multipart MJPEG) and `GET /frame.jpg` (auth required).
  Frames are QQVGA at ~3 fps over loss-tolerant datagrams (a dropped chunk just
  skips that frame).
- **`/onboard`** — **add a new device**: step-by-step instructions, plus a live
  list of newly-discovered sensors waiting to be claimed. A device that
  connects appears here (not on the dashboard) until you name it and add it —
  an approval gate so only your devices show up.
- **`/settings`** — change the password, set an **alert webhook**, and give
  devices friendly names.

## Alerts (webhook)

Set a webhook URL in Settings (or `AIOT_WEBHOOK_URL`). On every enter/leave the
app POSTs JSON — wire it to Slack, IFTTT, or your automation:

```json
{"device":"esp32s3-3f38","event":"enter","state":"present","score":180,"at":1783583992.7}
```

## Storage

SQLite (`backend/aiot.db`, override with `AIOT_DB`): device registry, the full
presence-event log, and settings — so history and configuration survive
restarts. The DB is the source of truth for history; the live feed is tailed
from its end on startup, so restarts never duplicate events.

## API

- `GET /api/devices` — live snapshot (auth required).
- `GET /api/history` — recent events per device (auth required).
- `GET /healthz` — public liveness.

## The feed

The picoquic receiver (`host_receiver/picoquic/picohttp/demoserver.c`) appends
one JSON object per line to `AIOT_FEED_LOG` (default `aiot_feed.jsonl`):

```json
{"t":123,"cid":"..","kind":"publish","topic":"aiot/esp32s3-3f38/register","payload":"{...}"}
{"t":124,"cid":"..","kind":"datagram","track":1,"seq":8,"gen_ts_ms":48312,"payload":"{...}"}
```

Swapping in a real MQTT-over-QUIC broker (EMQX / HiveMQ / Mosquitto) later only
changes how the feed is produced, not the app.
