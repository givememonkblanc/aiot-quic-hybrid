"""Exercise restored paper Paho/VolantMQ/QUIC fork with verified local TLS.

This is an author-code adaptation, not the exact paper hardware or workload.
Do not mix its combined-client CPU/Go allocations with the Rust broker's RSS.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from compare_author import ROOT, certs, free_port, terminate
from paper_metrics import delay_metrics, quantile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--count', type=int, default=100)
    parser.add_argument('--repetitions', type=int, default=3)
    args=parser.parse_args()
    if not (1<=args.count<=1000 and 1<=args.repetitions<=50):
        parser.error('Outside bounded workload')
    args.output=args.output.resolve()
    args.output.mkdir(parents=True,exist_ok=False)
    broker_bin=ROOT/'legacy_build/volantmq-lab'
    client_bin=ROOT/'legacy_build/legacy-client'
    manifest={'classification':'restored_author_code_with_TLS_and_listener_fixes_NOT_full_paper_reproduction',
              'network':'loopback; no loss/delay imposed','clock':'same-process monotonic',
              'tls_verification':True,'broker_sha256':hashlib.sha256(broker_bin.read_bytes()).hexdigest(),
              'client_sha256':hashlib.sha256(client_bin.read_bytes()).hexdigest(),
              'go_version':'1.14.15','quic':'author fork based on draft-era quic-go, not RFC9000',
              'count':args.count,'interval_ms':33,'repetitions':args.repetitions}
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    rows=[]
    with tempfile.TemporaryDirectory(prefix='legacy-mqtt-paper-') as temp:
        root=Path(temp);certs(root)
        shutil.copy(root/'server.pem',root/'cert.crt')
        shutil.copy(root/'server.key',root/'cert.key')
        for run in range(args.repetitions):
            for qos in (0,1,2):
                payload=256
                case=args.output/f'q{qos}-r{run}';case.mkdir()
                port=free_port(True)
                config=root/'config.yaml'
                config.write_text(f'''version: v0.0.1
system:
  log:
    console:
      level: error
plugins:
  enabled: []
auth:
  anonymous: true
listeners:
  defaultAddr: "127.0.0.1"
  mqtt:
    quic:
      {port}:
        def: ""
''')
                proc=None
                result={'run':run,'qos':qos,'payload_bytes':payload,'status':'failed'}
                try:
                    env=dict(os.environ,VOLANTMQ_WORK_DIR=str(root/'work'),GOMAXPROCS='2')
                    with (case/'broker.log').open('w') as log:
                        proc=subprocess.Popen([str(broker_bin),'--config',str(config)],cwd=root,
                                              env=env,stdout=log,stderr=subprocess.STDOUT)
                    time.sleep(.4)
                    if proc.poll() is not None:
                        raise RuntimeError(f'broker exited {proc.returncode}')
                    command=[str(client_bin),'-broker',f'quic://127.0.0.1:{port}',
                             '-ca',str(root/'ca.pem'),'-qos',str(qos),'-size',str(payload),
                             '-count',str(args.count),'-interval-ms','33']
                    (case/'command.json').write_text(json.dumps(command)+'\n')
                    with (case/'client.log').open('w') as out:
                        completed=subprocess.run(command,cwd=root,env=env,stdout=out,stderr=subprocess.STDOUT,
                                                 timeout=25+args.count*.033)
                    if completed.returncode:
                        raise RuntimeError(f'client exited {completed.returncode}')
                    lines=(case/'client.log').read_text().splitlines()
                    data=next(json.loads(line) for line in reversed(lines) if line.startswith('{"'))
                    values=data['latency_samples_ms']
                    data.update(delay_metrics(values))
                    data.update({f'latency_p{p}_ms':quantile(values,p) for p in (50,95,99)})
                    if data['received_unique']!=args.count:
                        raise RuntimeError('incomplete delivery')
                    result.update(data,status='ok')
                except Exception as error:
                    result['error']=f'{type(error).__name__}: {error}'
                finally:
                    terminate(proc)
                rows.append(result)
                (case/'result.json').write_text(json.dumps(result,indent=2)+'\n')
                (args.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
                print('legacy-quic',qos,run,result['status'],result.get('received_unique',result.get('error')),flush=True)
    if any(r['status']!='ok' for r in rows):
        raise SystemExit(1)


if __name__=='__main__':
    main()
