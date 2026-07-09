#!/usr/bin/env python3
"""AIoT Presence Sensor — backend + web app (login, dashboard, settings).

Tails the receiver's unified JSON-lines feed (aiot_feed.jsonl, written by the
picoquic MQTT-over-QUIC server) and serves a small product web app:

  * /login     — sign in (session cookie)
  * /          — live occupancy dashboard (auth required)
  * /settings  — account password, alert webhook, device names (auth required)

Presence transitions are persisted to SQLite (survive restarts), shown as
history + today's stats, and can fire a webhook / browser notification so
anything reacts to "someone entered / left".

    AIOT_PASS=secret python3 dashboard.py --port 8080

First run sets the admin password from AIOT_PASS (default "admin" — change it
in Settings). Stdlib only, no external dependencies.
"""
import argparse
import hashlib
import html
import json
import os
import secrets
import sqlite3
import threading
import time
import urllib.request
from datetime import datetime
from http import cookies as http_cookies
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

_lock = threading.Lock()
_devices = {}
_started = time.time()
_sessions = {}                 # token -> created_ts
_db = None
_settings = {}                 # cached: webhook_url, pw_hash, device_names(json)
_image_path = None             # latest.jpg written by the receiver (live view)


def read_image():
    if not _image_path:
        return None
    try:
        with open(_image_path, "rb") as f:
            data = f.read()
    except (FileNotFoundError, OSError):
        return None
    return data if data[:2] == b"\xff\xd8" else None   # only a complete JPEG

OFFLINE_AFTER_S = 20
MAX_SAMPLES = 160
SESSION_TTL = 7 * 24 * 3600
USER = os.environ.get("AIOT_USER", "admin")


# ---------------------------------------------------------------------------
# Persistence & settings
# ---------------------------------------------------------------------------
def db_init(path, default_pass):
    global _db
    _db = sqlite3.connect(path, check_same_thread=False)
    _db.executescript("""
        CREATE TABLE IF NOT EXISTS setting(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS device(device_id TEXT PRIMARY KEY, type TEXT,
            model TEXT, fw TEXT, manufacturer TEXT, hb_ms INTEGER, registered_at REAL);
        CREATE TABLE IF NOT EXISTS event(id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT, at REAL, from_state TEXT, to_state TEXT, dwell_s REAL, score INTEGER);
        CREATE INDEX IF NOT EXISTS ev_dev_at ON event(device_id, at);
    """)
    _db.commit()
    for k, v in _db.execute("SELECT key,value FROM setting"):
        _settings[k] = v
    if "pw_hash" not in _settings:
        set_password(default_pass)
    _settings.setdefault("webhook_url", os.environ.get("AIOT_WEBHOOK_URL", ""))


def set_setting(key, value):
    _settings[key] = value
    _db.execute("INSERT INTO setting(key,value) VALUES(?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, value))
    _db.commit()


def _hash_pw(pw, salt):
    return hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), 120000).hex()


def set_password(pw):
    salt = secrets.token_hex(16)
    set_setting("pw_hash", salt + ":" + _hash_pw(pw, salt))


def check_password(pw):
    stored = _settings.get("pw_hash", "")
    if ":" not in stored:
        return False
    salt, h = stored.split(":", 1)
    return secrets.compare_digest(_hash_pw(pw, salt), h)


def db_upsert_device(d):
    _db.execute("""INSERT INTO device(device_id,type,model,fw,manufacturer,hb_ms,registered_at)
        VALUES(?,?,?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET type=excluded.type,
        model=excluded.model,fw=excluded.fw,manufacturer=excluded.manufacturer,hb_ms=excluded.hb_ms""",
        (d["device_id"], d["type"], d["model"], d["fw"], d["manufacturer"], d["hb_ms"], d["registered_at"]))
    _db.commit()


def db_insert_event(dev, at, frm, to, dwell, score):
    _db.execute("INSERT INTO event(device_id,at,from_state,to_state,dwell_s,score) VALUES(?,?,?,?,?,?)",
                (dev, at, frm, to, dwell, score))
    _db.commit()


def db_load():
    for r in _db.execute("SELECT device_id,type,model,fw,manufacturer,hb_ms,registered_at FROM device"):
        d = _device(r[0])
        d["type"], d["model"], d["fw"], d["manufacturer"], d["hb_ms"], d["registered_at"] = r[1:]
    for dev in list(_devices):
        rows = _db.execute("SELECT at,from_state,to_state,dwell_s,score FROM event "
                           "WHERE device_id=? ORDER BY at DESC LIMIT 40", (dev,)).fetchall()
        _devices[dev]["events"] = [{"at": r[0], "from": r[1], "to": r[2], "dwell_s": r[3], "score": r[4]} for r in rows]


def device_names():
    try:
        return json.loads(_settings.get("device_names", "{}"))
    except ValueError:
        return {}


def claimed_set():
    try:
        return set(json.loads(_settings.get("claimed", "[]")))
    except ValueError:
        return set()


def claim_device(dev, name=""):
    claimed = claimed_set()
    claimed.add(dev)
    set_setting("claimed", json.dumps(sorted(claimed)))
    if name:
        names = device_names()
        names[dev] = name
        set_setting("device_names", json.dumps(names))


def today_stats(dev):
    midnight = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
    rows = _db.execute("SELECT at,to_state FROM event WHERE device_id=? AND at>=? ORDER BY at",
                       (dev, midnight)).fetchall()
    occ, visits, entered = 0.0, 0, None
    for at, to in rows:
        if to == "present":
            visits += 1
            entered = at
        elif to == "absent" and entered is not None:
            occ += at - entered
            entered = None
    d = _devices.get(dev)
    if entered is not None and d and d.get("state") == "present":
        occ += time.time() - entered
    return {"occupied_s": round(occ), "visits": visits}


# ---------------------------------------------------------------------------
# Alerts
# ---------------------------------------------------------------------------
def fire_webhook(dev, to_state, score, at):
    url = _settings.get("webhook_url", "")
    if not url:
        return
    payload = json.dumps({"device": dev, "event": "enter" if to_state == "present" else "leave",
                          "state": to_state, "score": score, "at": at}).encode()

    def _post():
        try:
            req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5).read()
        except Exception as e:  # noqa: BLE001
            print(f"[webhook] failed: {e}")
    threading.Thread(target=_post, daemon=True).start()


# ---------------------------------------------------------------------------
# In-memory model
# ---------------------------------------------------------------------------
def _device(dev):
    d = _devices.get(dev)
    if d is None:
        d = {"device_id": dev, "type": None, "model": None, "fw": None, "manufacturer": None,
             "state": "unknown", "score": None, "rssi": None, "heap": None, "hb_ms": None,
             "last_seen": 0.0, "registered_at": None, "heartbeats": 0, "events": [],
             "samples": [], "state_since": time.time()}
        _devices[dev] = d
    return d


def _set_state(d, new, now, score=None):
    if not new or new == d["state"]:
        return
    prev, dwell = d["state"], now - d["state_since"]
    d["events"].insert(0, {"at": now, "from": prev, "to": new, "dwell_s": round(dwell, 1), "score": score})
    del d["events"][40:]
    d["state"], d["state_since"] = new, now
    if prev != "unknown":
        db_insert_event(d["device_id"], now, prev, new, round(dwell, 1), score)
        fire_webhook(d["device_id"], new, score, now)


def _sample(d, now):
    d["samples"].append([round(now, 1), 1 if d["state"] == "present" else 0])
    del d["samples"][:-MAX_SAMPLES]


def _dev_id_from_topic(t):
    p = t.split("/")
    if len(p) >= 3 and p[0] == "aiot" and p[1] != "presence":
        return p[1]
    if t.startswith("homeassistant/") and len(p) >= 4:
        return p[2]
    return None


def _handle_publish(topic, payload, now):
    inner = None
    if payload:
        try:
            inner = json.loads(payload)
        except (ValueError, TypeError):
            inner = None
    dev = _dev_id_from_topic(topic) or (inner.get("device") if isinstance(inner, dict) else None)
    if not dev:
        return
    d = _device(dev)
    d["last_seen"] = now
    if topic.endswith("/register") and isinstance(inner, dict):
        for k, dk in (("type", "type"), ("model", "model"), ("fw", "fw"), ("mf", "manufacturer"), ("hb_ms", "hb_ms")):
            if inner.get(k) is not None:
                d[dk] = inner[k]
        if d["registered_at"] is None:
            d["registered_at"] = now
        db_upsert_device(d)
    elif topic.endswith("/status"):
        d["availability"] = payload
    elif topic.endswith("/state"):
        _set_state(d, payload, now)
        _sample(d, now)
    elif topic == "aiot/presence/event" and isinstance(inner, dict):
        _set_state(d, inner.get("state"), now, inner.get("score"))
        _sample(d, now)


def _handle_datagram(track, payload, now):
    if not payload:
        return
    try:
        inner = json.loads(payload)
    except (ValueError, TypeError):
        return
    if not isinstance(inner, dict) or not inner.get("device"):
        return
    d = _device(inner["device"])
    d["last_seen"] = now
    d["heartbeats"] += 1
    if inner.get("state"):
        _set_state(d, inner["state"], now, inner.get("score"))
    for k in ("score", "rssi", "heap"):
        if inner.get(k) is not None:
            d[k] = inner[k]
    _sample(d, now)


def _process_line(line, now):
    try:
        rec = json.loads(line)
    except ValueError:
        return
    if rec.get("kind") == "publish":
        _handle_publish(rec.get("topic", ""), rec.get("payload", ""), now)
    elif rec.get("kind") == "datagram":
        _handle_datagram(rec.get("track"), rec.get("payload", ""), now)


def tail_feed(path):
    # Start at the END of the feed: history is restored from the DB (the source
    # of truth), so restarts don't replay/duplicate old events. Live devices
    # reappear within one heartbeat.
    pos, inode = None, None
    while True:
        try:
            st = os.stat(path)
        except FileNotFoundError:
            time.sleep(0.5)
            continue
        if inode != st.st_ino:                 # new / rotated file: skip to end
            pos, inode = st.st_size, st.st_ino
        elif pos is None or st.st_size < pos:   # truncated: restart from 0
            pos = 0
        if st.st_size > pos:
            with open(path, "r", errors="replace") as f:
                f.seek(pos)
                chunk = f.read()
                pos = f.tell()
            now = time.time()
            with _lock:
                for line in chunk.splitlines():
                    if line.strip():
                        _process_line(line, now)
        time.sleep(0.4)


def snapshot():
    now = time.time()
    names = device_names()
    claimed = claimed_set()
    with _lock:
        out, pending, present = [], [], 0
        for d in _devices.values():
            age = now - d["last_seen"] if d["last_seen"] else None
            online = age is not None and age < OFFLINE_AFTER_S
            if d["device_id"] not in claimed:
                pending.append({"device_id": d["device_id"], "model": d["model"],
                                "fw": d["fw"], "online": online,
                                "age_s": round(age, 1) if age is not None else None})
                continue
            state = d["state"] if online else "unknown"
            if online and state == "present":
                present += 1
            out.append({**d, "state": state, "online": online,
                        "name": names.get(d["device_id"], ""),
                        "age_s": round(age, 1) if age is not None else None,
                        "since_s": round(now - d["state_since"], 1),
                        "today": today_stats(d["device_id"])})
        out.sort(key=lambda x: x["device_id"])
        pending.sort(key=lambda x: x["device_id"])
    return {"devices": out, "pending": pending, "present": present,
            "online": sum(1 for d in out if d["online"]), "server_uptime_s": round(now - _started, 1)}


# ---------------------------------------------------------------------------
# Web pages
# ---------------------------------------------------------------------------
CSS = r"""
:root{--bg:#0c1116;--panel:#121a22;--line:#20303c;--tx:#e7eef5;--dim:#8ca0b3;--faint:#5f7286;--accent:#38bdf8;
--present:#3fb950;--present-bg:#12351f;--vacant:#5f7488;--vacant-bg:#161d25;--on:#3fb950;--off:#f85149;
--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;--ui:system-ui,-apple-system,"Segoe UI",Roboto,"Apple SD Gothic Neo",sans-serif}
@media(prefers-color-scheme:light){:root{--bg:#eef2f6;--panel:#fff;--line:#dae2ea;--tx:#0d1a24;--dim:#4d6376;--faint:#7c93a6;--accent:#0284c7;--present-bg:#dcf5e3;--vacant-bg:#eef2f6}}
:root[data-theme=light]{--bg:#eef2f6;--panel:#fff;--line:#dae2ea;--tx:#0d1a24;--dim:#4d6376;--faint:#7c93a6;--accent:#0284c7;--present-bg:#dcf5e3;--vacant-bg:#eef2f6}
:root[data-theme=dark]{--bg:#0c1116;--panel:#121a22;--line:#20303c;--tx:#e7eef5;--dim:#8ca0b3;--accent:#38bdf8;--present-bg:#12351f;--vacant-bg:#161d25}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.5 var(--ui);-webkit-font-smoothing:antialiased}
a{color:var(--accent);text-decoration:none}
.wrap{max-width:1080px;margin:0 auto;padding:0 20px 60px}
header{display:flex;align-items:center;gap:13px;flex-wrap:wrap;padding:22px 0 18px;border-bottom:1px solid var(--line);margin-bottom:20px}
.logo{width:34px;height:34px;border-radius:9px;display:grid;place-items:center;background:linear-gradient(150deg,var(--accent),#0e7490);font-size:18px}
h1{font-size:18px;font-weight:650;margin:0}.osub{color:var(--dim);font-size:12.5px;margin-top:1px}
.nav{margin-left:auto;display:flex;align-items:center;gap:14px;font-size:13px;color:var(--dim)}
.beat{width:8px;height:8px;border-radius:50%;background:var(--on);animation:b 2.4s infinite}
@keyframes b{0%{box-shadow:0 0 0 0 rgba(63,185,80,.45)}70%{box-shadow:0 0 0 7px rgba(63,185,80,0)}100%{box-shadow:0 0 0 0 rgba(63,185,80,0)}}
.bell{cursor:pointer;border:1px solid var(--line);border-radius:7px;padding:3px 9px;font-size:12px;color:var(--dim);background:var(--panel)}
.bell.on{color:var(--accent);border-color:var(--accent)}
.tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:20px}
.tile{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:13px 16px}
.tile .n{font-size:26px;font-weight:680;font-variant-numeric:tabular-nums}.tile .l{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.06em;margin-top:1px}
.tile.hi .n{color:var(--present)}
.cards{display:grid;grid-template-columns:1fr;gap:14px}@media(min-width:720px){.cards{grid-template-columns:1fr 1fr}}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;overflow:hidden}
.hero{padding:18px;display:flex;align-items:center;gap:14px}.hero.present{background:var(--present-bg)}.hero.vacant{background:var(--vacant-bg)}
.dot{width:46px;height:46px;border-radius:50%;flex:none;display:grid;place-items:center;position:relative}
.dot i{width:16px;height:16px;border-radius:50%;background:var(--vacant)}.hero.present .dot i{background:var(--present)}
.hero.present .dot::after{content:"";position:absolute;inset:0;border-radius:50%;border:2px solid var(--present);animation:pulse 1.8s infinite}
@keyframes pulse{0%{transform:scale(.7);opacity:.9}100%{transform:scale(1.25);opacity:0}}
.big{font-size:22px;font-weight:720;line-height:1.1}.hero.present .big{color:var(--present)}.hero.vacant .big{color:var(--vacant)}
.since{color:var(--dim);font-size:12.5px;margin-top:3px;font-family:var(--mono)}
.head{margin-left:auto;text-align:right}.did{font-family:var(--mono);font-size:13px;font-weight:600}
.dname{font-size:14px;font-weight:600}
.avail{font-size:10.5px;font-weight:600;padding:2px 8px;border-radius:5px;margin-top:4px;display:inline-block;color:var(--on);background:rgba(63,185,80,.14)}
.avail.off{color:var(--off);background:rgba(248,81,73,.14)}
.today{display:flex;gap:18px;padding:10px 18px;border-top:1px solid var(--line);border-bottom:1px solid var(--line)}
.today div{font-size:12px;color:var(--dim)}.today b{display:block;font-size:16px;color:var(--tx);font-weight:700;font-variant-numeric:tabular-nums;font-family:var(--mono)}
.striplbl{display:flex;justify-content:space-between;padding:10px 18px 0;color:var(--faint);font-size:10px;text-transform:uppercase;letter-spacing:.05em}
.strip{height:32px;display:flex;gap:1px;padding:2px 18px 4px}.strip i{flex:1;border-radius:1px;background:var(--vacant);opacity:.35}.strip i.p{background:var(--present);opacity:.9}
.body{padding:12px 18px 16px}.meta{color:var(--dim);font-size:12px;font-family:var(--mono);margin-bottom:12px}
.grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:11px 16px}
.k{color:var(--faint);font-size:10px;text-transform:uppercase;letter-spacing:.05em}.v{font-weight:600;font-variant-numeric:tabular-nums;margin-top:1px;font-family:var(--mono);font-size:13.5px}
.ev{margin-top:14px;border-top:1px solid var(--line);padding-top:10px;max-height:104px;overflow:auto}
.ev .row{display:flex;justify-content:space-between;font-size:12px;color:var(--dim);padding:2px 0;font-family:var(--mono)}.ev .row b{color:var(--tx);font-weight:600}
.empty{color:var(--dim);text-align:center;padding:48px;grid-column:1/-1}
.center{min-height:100vh;display:grid;place-items:center;padding:20px}
.box{width:100%;max-width:360px;background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:28px}
.box h2{margin:0 0 4px;font-size:20px}.box p.sub{margin:0 0 20px;color:var(--dim);font-size:13px}
label{display:block;font-size:12px;color:var(--dim);margin:14px 0 5px;text-transform:uppercase;letter-spacing:.04em}
input{width:100%;padding:11px 12px;border:1px solid var(--line);border-radius:9px;font-size:15px;background:var(--bg);color:var(--tx)}
button{margin-top:20px;width:100%;padding:12px;background:var(--accent);color:#04141c;border:0;border-radius:9px;font-size:15px;font-weight:650;cursor:pointer}
.err{color:var(--off);font-size:13px;margin-top:12px}.ok{color:var(--present);font-size:13px;margin-top:12px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:20px;margin-bottom:16px}
.panel h3{margin:0 0 4px;font-size:15px}.panel .d{color:var(--dim);font-size:13px;margin:0 0 8px}
.warn{background:color-mix(in srgb,var(--off) 12%,transparent);border:1px solid var(--off);color:var(--off);border-radius:10px;padding:10px 14px;font-size:13px;margin-bottom:16px}
.banner{display:flex;align-items:center;gap:12px;background:color-mix(in srgb,var(--accent) 12%,transparent);border:1px solid var(--accent);border-radius:12px;padding:12px 16px;margin-bottom:18px;font-size:14px}
.banner a{margin-left:auto;background:var(--accent);color:#04141c;font-weight:650;padding:7px 14px;border-radius:8px;font-size:13px}
.steps{counter-reset:s;display:grid;gap:10px;margin:6px 0 0}
.step{display:grid;grid-template-columns:26px 1fr;gap:12px;align-items:start}
.step .sn{width:24px;height:24px;border-radius:50%;background:var(--accent);color:#04141c;font-weight:700;display:grid;place-items:center;font-size:13px}
.step .st{font-size:14px}.step .st small{color:var(--dim);display:block;font-size:12.5px}
.pend{display:flex;gap:10px;align-items:center;margin-top:12px;flex-wrap:wrap;padding-top:12px;border-top:1px solid var(--line)}
.pend:first-of-type{border-top:0}
.pinfo{min-width:150px}.pinfo .did{font-family:var(--mono);font-weight:600}.pinfo .pmeta{color:var(--dim);font-size:12px}
.pend input{flex:1;min-width:140px;margin:0}.pend button{margin:0;width:auto;padding:11px 18px}
.livecam{background:var(--panel);border:1px solid var(--line);border-radius:14px;overflow:hidden;margin-bottom:16px}
.livecam .lh{display:flex;align-items:center;gap:8px;padding:12px 16px;font-size:14px;font-weight:600}
.livecam .lh .rec{width:9px;height:9px;border-radius:50%;background:var(--off);animation:b 1.4s infinite}
.livecam .lh small{margin-left:auto;color:var(--dim);font-weight:400;font-size:12px}
.livecam .frame{background:#0a0e13;display:grid;place-items:center;min-height:180px;position:relative}
.livecam img{width:100%;max-height:60vh;object-fit:contain;display:block;image-rendering:auto}
.livecam .ph{position:absolute;color:var(--faint);font-size:13px}
"""

LOGIN_HTML = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>로그인 · AIoT Occupancy</title>
<style>%s</style></head><body><div class=center><form class=box method=post action=/login>
<h2>📡 AIoT Occupancy</h2><p class=sub>존재감지 대시보드에 로그인</p>
<label>아이디</label><input name=user value=admin autofocus autocapitalize=off>
<label>비밀번호</label><input name=pass type=password>
<button type=submit>로그인</button>%s</form></div></body></html>"""

DASHBOARD_HTML = r"""<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>AIoT Occupancy</title>
<style>__CSS__</style></head><body><div class=wrap>
<header><div class=logo>📡</div><div><h1>Occupancy</h1><div class=osub>AIoT presence sensors · MQTT-over-QUIC</div></div>
<div class=nav><span class=bell id=bell onclick=askNotif()>🔔 알림 켜기</span>
<a href=/onboard>장비 추가</a><a href=/settings>설정</a><a href=/logout>로그아웃</a><span class=beat></span><span id=live>connecting…</span></div></header>
<div id=banner></div>
<div class=livecam><div class=lh><span class=rec></span>라이브 영상 (실시간 카메라)<small id=camst>연결 중…</small></div>
<div class=frame><span class=ph id=camph>카메라 프레임 대기 중…</span>
<img id=cam alt="" style="display:none"></div></div>
<div class=tiles>
<div class="tile hi"><div class=n id=t-present>0</div><div class=l>Occupied now</div></div>
<div class=tile><div class=n id=t-online>0</div><div class=l>Online</div></div>
<div class=tile><div class=n id=t-total>0</div><div class=l>Devices</div></div></div>
<div class=cards id=grid><div class=empty>Waiting for a sensor to connect…</div></div></div>
<script>
let notif=false,lastState={};
function askNotif(){if(!("Notification"in window))return;Notification.requestPermission().then(p=>{notif=(p==="granted");
 let b=document.getElementById('bell');b.classList.toggle('on',notif);b.textContent=notif?'🔔 알림 켜짐':'🔔 알림 켜기';});}
function dur(s){if(s==null)return'—';s=Math.max(0,Math.round(s));if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+(s%60)+'s';return Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';}
function esc(x){return x==null?'—':(''+x);}
function strip(sm){const N=64,s=(sm||[]).slice(-N);let h='';for(let i=0;i<N-s.length;i++)h+='<i></i>';for(const x of s)h+='<i class="'+(x[1]?'p':'')+'"></i>';return h;}
async function tick(){let r;try{const q=await fetch('/api/devices');if(q.status===401){location='/login';return;}r=await q.json();}
 catch(e){document.getElementById('live').textContent='backend offline';return;}
 document.getElementById('live').textContent=new Date().toLocaleTimeString();
 document.getElementById('t-present').textContent=r.present;document.getElementById('t-online').textContent=r.online;document.getElementById('t-total').textContent=r.devices.length;
 const np=(r.pending||[]).length;
 document.getElementById('banner').innerHTML=np?`<div class=banner><span>🆕 새로 발견된 기기 <b>${np}대</b> — 추가하시겠어요?</span><a href=/onboard>장비 추가</a></div>`:'';
 for(const d of r.devices){const p=lastState[d.device_id];if(notif&&p&&p!=='present'&&d.state==='present')new Notification('사람 감지',{body:(d.name||d.device_id)+' — 누군가 들어왔습니다'});lastState[d.device_id]=d.state;}
 const g=document.getElementById('grid');
 if(!r.devices.length){g.innerHTML='<div class=empty>Waiting for a sensor to connect…</div>';return;}
 g.innerHTML=r.devices.map(d=>{const occ=d.state==='present';const label=!d.online?'NO SIGNAL':(occ?'OCCUPIED':'VACANT');
  const since=d.online?((occ?'occupied ':'vacant ')+dur(d.since_s)):('last seen '+dur(d.age_s)+' ago');
  const ev=(d.events||[]).slice(0,5).map(e=>`<div class=row><span><b>${e.to==='present'?'entered':'left'}</b>${e.score!=null?' · score '+e.score:''}</span><span>${e.from&&e.from!=='unknown'?dur(e.dwell_s)+' '+e.from+' · ':''}${dur((Date.now()/1000)-e.at)} ago</span></div>`).join('');
  const t=d.today||{};const title=d.name?`<div class=dname>${esc(d.name)}</div><div class=did>${esc(d.device_id)}</div>`:`<div class=did>${esc(d.device_id)}</div>`;
  return `<div class=card><div class="hero ${occ?'present':'vacant'}"><div class=dot><i></i></div>
   <div><div class=big>${label}</div><div class=since>${since}</div></div>
   <div class=head>${title}<span class="avail ${d.online?'':'off'}">${d.online?'online':'offline'}</span></div></div>
   <div class=today><div>오늘 점유<b>${dur(t.occupied_s)}</b></div><div>방문<b>${esc(t.visits)}회</b></div><div>하트비트<b>${esc(d.heartbeats)}</b></div></div>
   <div class=striplbl><span>presence history</span><span>now →</span></div><div class=strip>${strip(d.samples)}</div>
   <div class=body><div class=meta>${esc(d.model)} · fw ${esc(d.fw)} · ${esc(d.type)}</div>
    <div class=grid><div><div class=k>Motion score</div><div class=v>${esc(d.score)}</div></div>
     <div><div class=k>Wi-Fi RSSI</div><div class=v>${d.rssi!=null?d.rssi+' dBm':'—'}</div></div>
     <div><div class=k>Free heap</div><div class=v>${d.heap!=null?(d.heap/1048576).toFixed(1)+' MB':'—'}</div></div></div>
    ${ev?`<div class=ev>${ev}</div>`:''}</div></div>`}).join('');}
(function(){var img=document.getElementById('cam'),ph=document.getElementById('camph'),
 st=document.getElementById('camst'),rec=document.querySelector('.livecam .rec');
 function start(){img.src='/stream?t='+Date.now();}
 img.onload=function(){img.style.display='block';ph.style.display='none';st.textContent='실시간';rec.style.background='var(--present)';};
 img.onerror=function(){img.style.display='none';ph.style.display='block';ph.textContent='카메라 신호 없음 — 재시도 중…';st.textContent='재연결';rec.style.background='var(--off)';setTimeout(start,3000);};
 start();})();
tick();setInterval(tick,1500);
</script></body></html>""".replace("__CSS__", CSS)


def settings_html(msg=""):
    dn = device_names()
    default_pw = check_password(os.environ.get("AIOT_PASS", "admin"))
    warn = '<div class=warn>⚠️ 기본 비밀번호를 사용 중입니다. 아래에서 변경하세요.</div>' if default_pw else ''
    rows = ""
    with _lock:
        for dev in sorted(_devices):
            edev = html.escape(str(dev))
            rows += (f'<label>{edev}</label><input name="name_{edev}" value="{html.escape(str(dn.get(dev,"")))}" '
                     f'placeholder="예: 회의실 A">')
    okmsg = f'<p class=ok>{msg}</p>' if msg else ''
    return f"""<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>설정 · AIoT Occupancy</title>
<style>{CSS}</style></head><body><div class=wrap>
<header><div class=logo>📡</div><div><h1>설정</h1><div class=osub>계정 · 알림 · 기기</div></div>
<div class=nav><a href=/>← 대시보드</a><a href=/onboard>장비 추가</a><a href=/logout>로그아웃</a></div></header>
{warn}{okmsg}
<div class=panel><h3>비밀번호 변경</h3><p class=d>대시보드 로그인 비밀번호</p>
<form method=post action=/settings/password>
<label>새 비밀번호</label><input name=pass type=password required>
<label>새 비밀번호 확인</label><input name=pass2 type=password required>
<button type=submit>비밀번호 변경</button></form></div>
<div class=panel><h3>알림 (Webhook)</h3><p class=d>사람이 들어오거나 나갈 때 이 URL로 알림을 보냅니다 (Slack · IFTTT · 자동화).</p>
<form method=post action=/settings/webhook>
<label>Webhook URL</label><input name=url value="{html.escape(str(_settings.get('webhook_url','')))}" placeholder="https://hooks.slack.com/...">
<button type=submit>저장</button></form></div>
<div class=panel><h3>기기 이름</h3><p class=d>기기에 알아보기 쉬운 이름을 붙입니다 (예: 회의실 A).</p>
<form method=post action=/settings/names>{rows or '<p class=d>아직 연결된 기기가 없습니다.</p>'}
{'<button type=submit>이름 저장</button>' if rows else ''}</form></div>
</div></body></html>"""


def onboard_html(msg=""):
    pending = snapshot()["pending"]
    okmsg = f'<p class=ok>{msg}</p>' if msg else ''
    if pending:
        items = ""
        for p in pending:
            st = "온라인 · 방금 감지" if p["online"] else "오프라인"
            edid = html.escape(str(p["device_id"]))
            items += (f'<form class=pend method=post action=/onboard/claim>'
                      f'<input type=hidden name=dev value="{edid}">'
                      f'<div class=pinfo><div class=did>{edid}</div>'
                      f'<div class=pmeta>{html.escape(str(p.get("model") or "장비"))} · {st}</div></div>'
                      f'<input name=name placeholder="이름 (예: 회의실 A)" autocomplete=off>'
                      f'<button type=submit>추가</button></form>')
        pend = (f'<div class=panel><h3>발견된 새 기기 · {len(pending)}대</h3>'
                f'<p class=d>추가할 기기의 이름을 정하고 [추가]를 누르면 대시보드에 나타납니다.</p>{items}</div>')
    else:
        pend = ('<div class=panel><h3>발견된 새 기기</h3>'
                '<p class=d>⏳ 대기 중… 새 기기를 켜고 아래 순서로 Wi-Fi를 설정하면 몇 초 뒤 여기에 자동으로 나타납니다.</p></div>')
    return f"""<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>장비 추가 · AIoT Occupancy</title>
<style>{CSS}</style></head><body><div class=wrap>
<header><div class=logo>📡</div><div><h1>장비 추가</h1><div class=osub>새 존재감지 센서 연동</div></div>
<div class=nav><a href=/>← 대시보드</a><a href=/settings>설정</a><a href=/logout>로그아웃</a></div></header>
{okmsg}{pend}
<div class=panel><h3>추가하는 방법</h3><div class=steps>
<div class=step><div class=sn>1</div><div class=st>새 기기에 전원(USB)을 연결합니다.<small>설정 안 된 기기는 스스로 설정 모드로 켜집니다 (LED 파랑).</small></div></div>
<div class=step><div class=sn>2</div><div class=st>폰 Wi-Fi에서 <b>AIoT-Presence-XXXX</b> 에 연결합니다.<small>설정 페이지가 자동으로 열립니다 (또는 http://192.168.4.1).</small></div></div>
<div class=step><div class=sn>3</div><div class=st>집/사무실 <b>Wi-Fi 이름과 비밀번호</b>만 입력하고 저장합니다.<small>서버 주소는 입력하지 않아도 됩니다 — 자동 연결됩니다.</small></div></div>
<div class=step><div class=sn>4</div><div class=st>몇 초 뒤 이 화면 위 <b>“발견된 새 기기”</b> 에 나타나면 이름을 지어 추가합니다.</div></div>
</div></div>
</div><script>setTimeout(()=>location.reload(),4000)</script></body></html>"""


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
def new_session():
    tok = secrets.token_hex(24)
    _sessions[tok] = time.time()
    return tok


class Handler(BaseHTTPRequestHandler):
    def _authed(self):
        c = self.headers.get("Cookie")
        if not c:
            return False
        jar = http_cookies.SimpleCookie(c)
        if "sid" not in jar:
            return False
        tok = jar["sid"].value
        created = _sessions.get(tok)
        if created is None or time.time() - created > SESSION_TTL:
            _sessions.pop(tok, None)
            return False
        return True

    def _send(self, code, body, ctype="text/html; charset=utf-8", headers=None):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or []):
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _redirect(self, loc, headers=None):
        self.send_response(302)
        self.send_header("Location", loc)
        for k, v in (headers or []):
            self.send_header(k, v)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _body(self):
        n = int(self.headers.get("Content-Length", 0))
        return parse_qs(self.rfile.read(n).decode(errors="replace")) if n else {}

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        if p == "/healthz":
            return self._send(200, "ok", "text/plain")
        if p == "/login":
            return self._send(200, LOGIN_HTML % (CSS, ""))
        if p == "/logout":
            c = self.headers.get("Cookie")
            if c:
                jar = http_cookies.SimpleCookie(c)
                if "sid" in jar:
                    _sessions.pop(jar["sid"].value, None)
            return self._redirect("/login", [("Set-Cookie", "sid=; Max-Age=0; Path=/")])
        # protected
        if not self._authed():
            if p.startswith("/api/"):
                return self._send(401, '{"error":"auth"}', "application/json")
            return self._redirect("/login")
        if p in ("/", "/index.html"):
            return self._send(200, DASHBOARD_HTML)
        if p == "/settings":
            return self._send(200, settings_html())
        if p == "/onboard":
            return self._send(200, onboard_html())
        if p == "/api/devices":
            return self._send(200, json.dumps(snapshot()), "application/json")
        if p == "/api/history":
            with _lock:
                hist = {dev: _devices[dev]["events"] for dev in _devices}
            return self._send(200, json.dumps(hist), "application/json")
        if p == "/frame.jpg":
            data = read_image()
            if data:
                return self._send(200, data, "image/jpeg")
            return self._send(503, "no frame yet", "text/plain")
        if p == "/stream":
            return self._mjpeg()
        return self._send(404, "not found", "text/plain")

    def _mjpeg(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        last = None
        try:
            while True:
                data = read_image()
                if data is not None and data != last:
                    last = data
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                     b"Content-Length: " + str(len(data)).encode() + b"\r\n\r\n")
                    self.wfile.write(data)
                    self.wfile.write(b"\r\n")
                time.sleep(0.08)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def do_POST(self):
        p = self.path.split("?", 1)[0]
        form = self._body()

        def one(k):
            return form.get(k, [""])[0]

        if p == "/login":
            if one("user") == USER and check_password(one("pass")):
                tok = new_session()
                return self._redirect("/", [("Set-Cookie", f"sid={tok}; HttpOnly; Path=/; Max-Age={SESSION_TTL}; SameSite=Lax")])
            return self._send(401, LOGIN_HTML % (CSS, '<p class=err>아이디 또는 비밀번호가 올바르지 않습니다.</p>'))

        if not self._authed():
            return self._redirect("/login")

        if p == "/settings/password":
            pw, pw2 = one("pass"), one("pass2")
            if len(pw) < 4 or pw != pw2:
                return self._send(200, settings_html())
            set_password(pw)
            return self._send(200, settings_html("비밀번호를 변경했습니다."))
        if p == "/settings/webhook":
            set_setting("webhook_url", one("url").strip())
            return self._send(200, settings_html("알림 Webhook을 저장했습니다."))
        if p == "/settings/names":
            names = device_names()
            for k, v in form.items():
                if k.startswith("name_"):
                    dev, val = k[5:], v[0].strip()
                    if val:
                        names[dev] = val
                    else:
                        names.pop(dev, None)
            set_setting("device_names", json.dumps(names))
            return self._send(200, settings_html("기기 이름을 저장했습니다."))
        if p == "/onboard/claim":
            dev = one("dev")
            if dev:
                claim_device(dev, one("name").strip())
                return self._send(200, onboard_html(f"'{one('name').strip() or dev}' 을(를) 추가했습니다."))
            return self._send(200, onboard_html())
        return self._send(404, "not found", "text/plain")

    def log_message(self, *a):
        pass


def main():
    global _image_path
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--feed", default=os.environ.get("AIOT_FEED_LOG", "aiot_feed.jsonl"))
    ap.add_argument("--db", default=os.environ.get("AIOT_DB", os.path.join(os.path.dirname(__file__), "aiot.db")))
    ap.add_argument("--image", default=os.environ.get("AIOT_IMAGE", os.path.join(os.path.dirname(__file__), "latest.jpg")))
    args = ap.parse_args()
    _image_path = args.image

    db_init(args.db, os.environ.get("AIOT_PASS", "admin"))
    with _lock:
        db_load()
    threading.Thread(target=tail_feed, args=(args.feed,), daemon=True).start()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[dashboard] feed={args.feed} db={args.db} user={USER} http://{args.host}:{args.port}/")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
