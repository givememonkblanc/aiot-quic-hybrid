"""Fixed JPEG core/priority diagnostic. No network shaping; restore live mode."""
import argparse, hashlib, json, time
from pathlib import Path
from datetime import datetime, timezone
import serial
ROOT=Path(__file__).resolve().parents[2]
BASE=ROOT/'experiments/aiot_resource_management'
FEED=ROOT/'experiments/handoff/results/cross-ap-final-mode2-03/feed.jsonl'

def main():
    p=argparse.ArgumentParser();p.add_argument('--core',type=int,required=True);p.add_argument('--seconds',type=int,default=90);p.add_argument('--priorities',default='0,1,5,6');a=p.parse_args()
    out=BASE/'results'/datetime.now(timezone.utc).strftime(f'sweep-core{a.core}-%Y%m%dT%H%M%SZ');out.mkdir()
    meta=dict(core=a.core,seconds=a.seconds,settle_seconds=15,fixture=json.loads((BASE/'fixtures/manifest.json').read_text()),stages=[],status='running')
    raw=(out/'serial.log').open('wb');records=(out/'records.jsonl').open('w');feed=FEED.open('rb');feed.seek(0,2);copied=(out/'feed.jsonl').open('wb')
    ser=serial.Serial(timeout=.15);ser.port='/dev/ttyACM0';ser.baudrate=115200;ser.dtr=False;ser.rts=False;ser.open()
    def cmd(s):ser.write((s+'\n').encode());ser.flush()
    def read(stage=-1):
        b=ser.readline();now=time.monotonic()
        if b:raw.write(b);raw.flush()
        s=b.decode(errors='replace')
        for tag in ('PROFILE','TASK_PROFILE','AI_START','AI_OFFER','AI_PROFILE','AI_CPU','AI_PHASE','IMG_PROFILE','REPLAY'):
            marker='['+tag+'] '
            if marker in s:
                try:r=json.loads(s.split(marker,1)[1])
                except ValueError:break
                records.write(json.dumps(dict(stage=stage,host=now,tag=tag,data=r))+'\n');records.flush()
                return tag,r
        return s,None
    def drain(seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:read();copied.write(feed.read())
    try:
        drain(8)
        for c in ('mode 2','policy 1','video 30 6144','probe 1','profile 5'):cmd(c);drain(.4)
        for idx,priority in enumerate(map(int,a.priorities.split(','))):
            cmd(f'aiprio {priority}');cmd('replay 1')
            start=time.monotonic();stage=dict(index=idx,priority=priority,start=start,status='running');meta['stages'].append(stage)
            print(f'{out.name} priority={priority} start',flush=True)
            last=start;prev=None;verified=False
            while time.monotonic()-start<a.seconds:
                tag,r=read(idx);now=time.monotonic();copied.write(feed.read())
                if tag=='PROFILE':
                    last=now
                    if prev is not None and r['uptime_us']<prev:raise RuntimeError('Device reboot during stage')
                    prev=r['uptime_us']
                    if r['internal_free']<16384 or r['psram_free']<65536:raise RuntimeError('Heap guard')
                if tag=='AI_START' and now-start>15 and r['priority']==priority:verified=True
                if now-last>20:raise RuntimeError('Profile telemetry missing 20s')
                if isinstance(tag,str) and ('Task watchdog got triggered' in tag or "Guru Meditation" in tag):raise RuntimeError('Watchdog/panic')
            stage.update(end=time.monotonic(),status='completed',priority_verified=verified)
            if not verified:raise RuntimeError('Effective AI priority not verified')
        meta['status']='completed'
    except Exception as e:
        meta.update(status='stopped',reason=str(e))
        if meta['stages']:meta['stages'][-1].update(end=time.monotonic(),status='stopped')
    finally:
        for c in ('aiprio 0','replay 0','probe 0','profile 0','mode 2','policy 1','video 30 6144'):cmd(c);drain(.3)
        drain(8);ser.close();raw.close();records.close();copied.close();feed.close()
        (out/'outcome.json').write_text(json.dumps(meta,indent=2)+'\n')
    print(out,flush=True);print(json.dumps(meta),flush=True)
if __name__=='__main__':main()
