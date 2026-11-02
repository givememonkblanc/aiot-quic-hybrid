"""A-E active-workload characterization; warmed allocations and live input."""
import argparse
from flow_shaper import FlowShaper
import csv
import hashlib
import json
import time
from datetime import datetime, timezone
from pathlib import Path
import serial
ROOT=Path(__file__).resolve().parents[2]
FEED=ROOT/'experiments/handoff/results/cross-ap-final-mode2-03'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--causal',action='store_true')
    parser.add_argument('--pin-test',action='store_true')
    args=parser.parse_args()
    prefix='causal' if args.causal else ('pin-test' if args.pin_test else 'profiles')
    shaper=FlowShaper()
    out=ROOT/'experiments/aiot_resource_management/results'/datetime.now(timezone.utc).strftime(prefix+'-%Y%m%dT%H%M%SZ')
    out.mkdir(parents=True)
    initial=json.loads((ROOT/'experiments/memory/results/latest.json').read_text())
    if time.time()-datetime.fromisoformat(initial['received_utc']).timestamp()>25:
        raise RuntimeError('Stale baseline telemetry')
    (out/'initial.json').write_text(json.dumps(initial,indent=2))
    sources=[FEED/'mqtt.csv',FEED/'datagrams.csv']
    offsets={p:p.stat().st_size for p in sources}
    headers={p:p.open().readline().encode() for p in sources}
    outcome={'status':'running','stages':[],'seconds_per_stage':45,'settle_seconds':15,'mode':2,'policy':1,'fps_target':30,'queue_budget_kib':6144,'firmware_sha256':hashlib.sha256((ROOT/'build_mobility_ota/aiot_quic.bin').read_bytes()).hexdigest()}
    ser=serial.Serial(timeout=.3);ser.port='/dev/ttyACM0';ser.baudrate=115200;ser.dtr=False;ser.rts=False;ser.open()
    rows=[];ais=[];previous=0
    outcome['experiment']=prefix
    outcome['probe_hz']=1 if args.causal or args.pin_test else 0
    raw=(out/'serial.log').open('wb')
    def command(s):
        ser.write((s+'\n').encode());ser.flush()
    def line():
        b=ser.readline()
        if b: raw.write(b);raw.flush()
        return b.decode(errors='replace')
    try:
        command('\nmode 2');time.sleep(.3);command('policy 1');time.sleep(.3);command('video 30 6144');time.sleep(.3)
        command('probe 1' if args.causal or args.pin_test else 'probe 0')
        stages=([3,6,5,5,5,5,6,3] if args.causal else ([3,5,5,3] if args.pin_test else [1,2,3,4,5,5,4,3,2,1]))
        for index,profile in enumerate(stages):
            impaired=args.causal and index in (3,4)
            if impaired: shaper.on()
            command(f'profile {profile}')
            start=time.monotonic();last_sample=start
            stage={'index':index,'profile':profile,'start_monotonic':start,'network_limited':impaired}
            outcome['stages'].append(stage)
            print(f'stage={index} profile={profile} started',flush=True)
            while time.monotonic()-start<45:
                s=line();now=time.monotonic()
                if '[PROFILE] ' in s:
                    try:r=json.loads(s.split('[PROFILE] ',1)[1])
                    except json.JSONDecodeError:continue
                    last_sample=now
                    if r['uptime_us']<previous or r['internal_free']<16384 or r['psram_free']<65536:
                        raise RuntimeError('Reboot/heap guard')
                    previous=r['uptime_us']
                    rows.append(dict(stage=index,host_monotonic=now,phase_s=now-start,**r))
                elif '[AI_PROFILE] ' in s:
                    try:r=json.loads(s.split('[AI_PROFILE] ',1)[1])
                    except json.JSONDecodeError:continue
                    ais.append(dict(stage=index,host_monotonic=now,phase_s=now-start,**r))
                if now-last_sample>20:raise RuntimeError('Profile telemetry missing for 20 seconds')
            stage['end_monotonic']=time.monotonic()
            if impaired:
                stage['netem_stats']=shaper.stats();shaper.off()
            if not any(r['stage']==index and r['profile']==profile and r['phase_s']>15 for r in rows):
                raise RuntimeError('Profile not acknowledged')
        outcome['status']='completed'
    except Exception as e:
        outcome.update(status='stopped',reason=str(e))
    finally:
        shaper.off()
        command('probe 0');time.sleep(.3)
        command('profile 0');time.sleep(.3)
        command(f'mode {initial["mode"]}');time.sleep(.3)
        command(f'policy {initial["video_policy"]}');time.sleep(.3)
        command(f'video {initial["target_fps"]} {initial["queue_budget"]//1024}')
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            if '[OK] profile=0' in line():outcome['profile_restore_ack']=True;break
        ser.close();raw.close()
        (out/'samples.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
        (out/'inferences.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in ais))
        for p in sources:
            with p.open('rb') as f:f.seek(offsets[p]);data=f.read()
            (out/p.name).write_bytes(headers[p]+data[:data.rfind(b'\n')+1])
        (out/'outcome.json').write_text(json.dumps(outcome,indent=2))
    print(out,flush=True)
    print(json.dumps(outcome),flush=True)

if __name__=='__main__':main()
