#!/usr/bin/env python3
"""AIoT Spaces — a space-utilization service built on the presence sensors.

A separate, standalone service (its own process/port) that *applies* the
installed devices to a real use case: find a free room right now, and see how
each space is actually used (today's occupancy %, visits, and an hourly
heatmap). It reuses the same data the management app produces — reads the
shared SQLite DB for the room list (claimed + named devices) and the event
history, and tails the same feed for live occupancy.

    AIOT_DB=backend/aiot.db AIOT_FEED_LOG=aiot_feed.jsonl \
    python3 spaceapp.py --port 8090

Stdlib only. Read-only over the shared data; run it alongside dashboard.py.
"""
import argparse
import json
import os
import sqlite3
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_lock = threading.Lock()
_live = {}          # device_id -> {"state","last_seen"}
_db = None
OFFLINE_AFTER_S = 20


# ---------------------------------------------------------------------------
# Shared data (read-only)
# ---------------------------------------------------------------------------
def rooms():
    """Claimed + named devices are the rooms this service shows."""
    try:
        claimed = set(json.loads(_get("claimed", "[]")))
        names = json.loads(_get("device_names", "{}"))
    except ValueError:
        claimed, names = set(), {}
    return [(d, names.get(d, d)) for d in sorted(claimed)]


def _get(key, default=""):
    r = _db.execute("SELECT value FROM setting WHERE key=?", (key,)).fetchone()
    return r[0] if r else default


def _midnight():
    return datetime.now().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()


def present_intervals_today(dev, now):
    mid = _midnight()
    rows = _db.execute("SELECT at,to_state FROM event WHERE device_id=? AND at>=? ORDER BY at",
                       (dev, mid)).fetchall()
    intervals, entered = [], None
    for at, to in rows:
        if to == "present":
            entered = at
        elif to == "absent" and entered is not None:
            intervals.append((entered, at))
            entered = None
    if entered is not None:
        intervals.append((entered, now))
    return intervals


def utilization(dev, now):
    mid = _midnight()
    ints = present_intervals_today(dev, now)
    occupied = sum(e - s for s, e in ints)
    elapsed = max(1.0, now - mid)
    visits = sum(1 for s, e in ints)
    hourly = []
    for h in range(24):
        h0, h1 = mid + h * 3600, mid + (h + 1) * 3600
        if h0 > now:
            hourly.append(None)
            continue
        span = min(h1, now) - h0
        occ = sum(max(0.0, min(e, h1) - max(s, h0)) for s, e in ints)
        hourly.append(round(occ / span, 3) if span > 0 else 0.0)
    peak = max((h for h in range(24) if hourly[h] is not None),
               key=lambda h: hourly[h], default=None)
    return {"occupied_s": round(occupied), "occupancy_pct": round(occupied / elapsed * 100),
            "visits": visits, "hourly": hourly,
            "peak_hour": peak if (peak is not None and hourly[peak]) else None}


def snapshot():
    now = time.time()
    out = []
    with _lock:
        for dev, name in rooms():
            lv = _live.get(dev, {})
            age = now - lv["last_seen"] if lv.get("last_seen") else None
            online = age is not None and age < OFFLINE_AFTER_S
            state = lv.get("state", "unknown") if online else "offline"
            out.append({"device_id": dev, "name": name, "state": state, "online": online,
                        "since_s": round(now - lv["state_since"], 1) if lv.get("state_since") else None,
                        **utilization(dev, now)})
    free = [r for r in out if r["state"] == "absent"]
    busy = [r for r in out if r["state"] == "present"]
    avg = round(sum(r["occupancy_pct"] for r in out) / len(out)) if out else 0
    most = max(out, key=lambda r: r["occupancy_pct"], default=None)
    return {"rooms": out, "free": len(free), "busy": len(busy), "total": len(out),
            "avg_pct": avg, "busiest": most["name"] if most and most["occupancy_pct"] else None}


# ---------------------------------------------------------------------------
# Live occupancy from the feed
# ---------------------------------------------------------------------------
def _set_live(dev, state, now):
    lv = _live.setdefault(dev, {})
    lv["last_seen"] = now
    if state and lv.get("state") != state:
        lv["state"] = state
        lv["state_since"] = now


def _feed_line(line, now):
    try:
        rec = json.loads(line)
    except ValueError:
        return
    if rec.get("kind") == "publish":
        topic, payload = rec.get("topic", ""), rec.get("payload", "")
        inner = {}
        try:
            inner = json.loads(payload)
        except (ValueError, TypeError):
            pass
        p = topic.split("/")
        dev = p[1] if len(p) >= 3 and p[0] == "aiot" and p[1] != "presence" else inner.get("device")
        if not dev:
            return
        if topic.endswith("/state"):
            _set_live(dev, payload, now)
        elif topic == "aiot/presence/event" and isinstance(inner, dict):
            _set_live(dev, inner.get("state"), now)
    elif rec.get("kind") == "datagram":
        try:
            inner = json.loads(rec.get("payload", ""))
        except (ValueError, TypeError):
            return
        if isinstance(inner, dict) and inner.get("device"):
            _set_live(inner["device"], inner.get("state"), now)


def tail_feed(path):
    pos, inode = None, None
    while True:
        try:
            st = os.stat(path)
        except FileNotFoundError:
            time.sleep(0.5)
            continue
        if inode != st.st_ino:
            pos, inode = st.st_size, st.st_ino
        elif pos is None or st.st_size < pos:
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
                        _feed_line(line, now)
        time.sleep(0.4)


# ---------------------------------------------------------------------------
# Web page
# ---------------------------------------------------------------------------
PAGE = r"""<!doctype html><html lang=ko><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Spaces · 공간 현황</title>
<style>
:root{--bg:#0d1117;--panel:#161d27;--line:#232e3b;--tx:#e7eef5;--dim:#8ca0b3;--faint:#5f7286;
--accent:#7c9cff;--free:#3fb950;--free-bg:#12351f;--busy:#e0a341;--busy-bg:#33260c;--off:#5f7286;
--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;--ui:system-ui,-apple-system,"Segoe UI",Roboto,"Apple SD Gothic Neo",sans-serif}
@media(prefers-color-scheme:light){:root{--bg:#f2f5fa;--panel:#fff;--line:#e0e7f0;--tx:#0e1826;--dim:#54677c;--faint:#8496a8;--accent:#4f6ef2;--free-bg:#ddf5e4;--busy-bg:#fbf0d8}}
:root[data-theme=light]{--bg:#f2f5fa;--panel:#fff;--line:#e0e7f0;--tx:#0e1826;--dim:#54677c;--faint:#8496a8;--accent:#4f6ef2;--free-bg:#ddf5e4;--busy-bg:#fbf0d8}
:root[data-theme=dark]{--bg:#0d1117;--panel:#161d27;--line:#232e3b;--tx:#e7eef5;--dim:#8ca0b3;--accent:#7c9cff;--free-bg:#12351f;--busy-bg:#33260c}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.5 var(--ui);-webkit-font-smoothing:antialiased}
.wrap{max-width:1000px;margin:0 auto;padding:0 20px 60px}
header{display:flex;align-items:center;gap:13px;padding:22px 0 18px;border-bottom:1px solid var(--line);margin-bottom:22px}
.logo{width:34px;height:34px;border-radius:9px;display:grid;place-items:center;background:linear-gradient(150deg,var(--accent),#3f51b5);font-size:18px}
h1{font-size:18px;font-weight:650;margin:0}.osub{color:var(--dim);font-size:12.5px}
.live{margin-left:auto;font-size:12px;color:var(--dim)}
.hero{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:24px;margin-bottom:20px;display:flex;align-items:center;gap:24px;flex-wrap:wrap}
.hero .big{font-size:46px;font-weight:760;line-height:1;color:var(--free);font-variant-numeric:tabular-nums}
.hero .lbl{color:var(--dim);font-size:14px;margin-top:4px}
.chips{display:flex;gap:8px;flex-wrap:wrap;margin-left:auto}
.chip{font-size:13px;font-weight:600;padding:7px 13px;border-radius:999px;border:1px solid var(--line)}
.chip.free{color:var(--free);background:var(--free-bg)}.chip.busy{color:var(--busy);background:var(--busy-bg)}
.kpis{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:22px}
.kpi{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:13px 16px}
.kpi .n{font-size:22px;font-weight:700;font-variant-numeric:tabular-nums}.kpi .l{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.05em;margin-top:2px}
.rooms{display:grid;grid-template-columns:1fr;gap:14px}@media(min-width:680px){.rooms{grid-template-columns:1fr 1fr}}
.room{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:18px;border-left:4px solid var(--off)}
.room.free{border-left-color:var(--free)}.room.busy{border-left-color:var(--busy)}
.rtop{display:flex;align-items:center;gap:10px}
.rname{font-size:17px;font-weight:700}
.badge{margin-left:auto;font-size:12px;font-weight:700;padding:4px 11px;border-radius:999px}
.badge.free{color:var(--free);background:var(--free-bg)}.badge.busy{color:var(--busy);background:var(--busy-bg)}.badge.off{color:var(--off);background:var(--line)}
.rsince{color:var(--dim);font-size:12.5px;font-family:var(--mono);margin-top:3px}
.util{margin-top:16px}.util .row{display:flex;justify-content:space-between;font-size:12px;color:var(--dim);margin-bottom:5px}
.bar{height:8px;border-radius:5px;background:var(--line);overflow:hidden}.bar i{display:block;height:100%;background:var(--accent);border-radius:5px}
.rmeta{display:flex;gap:18px;margin-top:14px}.rmeta div{font-size:12px;color:var(--dim)}.rmeta b{display:block;font-size:15px;color:var(--tx);font-weight:700;font-family:var(--mono)}
.heat{margin-top:16px}.heat .hl{color:var(--faint);font-size:10px;text-transform:uppercase;letter-spacing:.05em;margin-bottom:5px;display:flex;justify-content:space-between}
.hrs{display:flex;gap:2px}.hrs i{flex:1;height:26px;border-radius:2px;background:var(--line)}
.empty{color:var(--dim);text-align:center;padding:50px;grid-column:1/-1}
.empty a{color:var(--accent)}
</style></head><body><div class=wrap>
<header><div class=logo>🏢</div><div><h1>Spaces</h1><div class=osub>공간 현황 &amp; 이용률 · 존재감지 응용 서비스</div></div>
<span class=live id=live>…</span></header>
<div class=hero><div><div class=big id=free>0</div><div class=lbl>지금 사용 가능한 공간</div></div>
<div class=chips id=chips></div></div>
<div class=kpis>
<div class=kpi><div class=n id=k-total>0</div><div class=l>전체 공간</div></div>
<div class=kpi><div class=n id=k-avg>0%</div><div class=l>오늘 평균 이용률</div></div>
<div class=kpi><div class=n id=k-busy>—</div><div class=l>가장 많이 쓴 공간</div></div></div>
<div class=rooms id=rooms><div class=empty>등록된 공간이 없습니다. 관리 대시보드의 <b>장비 추가</b>에서 기기를 방으로 등록하세요.</div></div>
</div>
<script>
function dur(s){if(s==null)return'—';s=Math.max(0,Math.round(s));if(s<60)return s+'초';if(s<3600)return Math.floor(s/60)+'분';return Math.floor(s/3600)+'시간 '+Math.floor(s%3600/60)+'분';}
function heat(h){const c=getComputedStyle(document.documentElement);return (h||[]).map(v=>{
 if(v==null)return '<i></i>';const a=(0.12+v*0.88).toFixed(2);
 return `<i style="background:color-mix(in srgb,var(--accent) ${Math.round(v*100)}%,var(--line))" title="${Math.round(v*100)}%"></i>`;}).join('');}
async function tick(){let r;try{r=await(await fetch('/api/spaces')).json();}catch(e){document.getElementById('live').textContent='오프라인';return;}
 document.getElementById('live').textContent='업데이트 '+new Date().toLocaleTimeString();
 document.getElementById('free').textContent=r.free;
 document.getElementById('k-total').textContent=r.total;
 document.getElementById('k-avg').textContent=r.avg_pct+'%';
 document.getElementById('k-busy').textContent=r.busiest||'—';
 document.getElementById('chips').innerHTML=
  (r.free?`<span class="chip free">🟢 사용 가능 ${r.free}</span>`:'')+
  (r.busy?`<span class="chip busy">🟠 사용 중 ${r.busy}</span>`:'');
 const g=document.getElementById('rooms');
 if(!r.rooms.length){return;}
 g.innerHTML=r.rooms.map(rm=>{const cl=rm.state==='absent'?'free':(rm.state==='present'?'busy':'off');
  const badge=rm.state==='absent'?'<span class="badge free">비어있음</span>':(rm.state==='present'?'<span class="badge busy">사용 중</span>':'<span class="badge off">오프라인</span>');
  const since=rm.online?((rm.state==='present'?'사용 ':'비어있음 ')+dur(rm.since_s)):'신호 없음';
  const peak=rm.peak_hour!=null?`${rm.peak_hour}시`:'—';
  return `<div class="room ${cl}"><div class=rtop><div><div class=rname>${rm.name}</div><div class=rsince>${since}</div></div>${badge}</div>
   <div class=util><div class=row><span>오늘 이용률</span><span>${rm.occupancy_pct}%</span></div><div class=bar><i style="width:${Math.min(100,rm.occupancy_pct)}%"></i></div></div>
   <div class=rmeta><div>오늘 점유<b>${dur(rm.occupied_s)}</b></div><div>방문<b>${rm.visits}회</b></div><div>피크<b>${peak}</b></div></div>
   <div class=heat><div class=hl><span>시간대별 점유 (0–23시)</span><span>지금 →</span></div><div class=hrs>${heat(rm.hourly)}</div></div>
  </div>`}).join('');}
tick();setInterval(tick,2000);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        if p in ("/", "/index.html"):
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif p == "/api/spaces":
            self._send(200, json.dumps(snapshot()), "application/json")
        elif p == "/healthz":
            self._send(200, "ok", "text/plain")
        else:
            self._send(404, "not found", "text/plain")

    def log_message(self, *a):
        pass


def main():
    global _db
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8090)
    # No auth on this service — default to localhost. Expose remotely only
    # behind an authenticated reverse proxy / SSH tunnel (override with --host).
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--db", default=os.environ.get("AIOT_DB", os.path.join(os.path.dirname(__file__), "aiot.db")))
    ap.add_argument("--feed", default=os.environ.get("AIOT_FEED_LOG", "aiot_feed.jsonl"))
    args = ap.parse_args()
    _db = sqlite3.connect(args.db, check_same_thread=False)
    threading.Thread(target=tail_feed, args=(args.feed,), daemon=True).start()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[spaces] db={args.db} feed={args.feed} http://{args.host}:{args.port}/")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
