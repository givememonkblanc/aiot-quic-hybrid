"""Run the remaining unshaped fixed-JPEG diagnostic cells, restoring each trial.
Not the gated 54-cell mission matrix; one replication, actual local inference.
"""
import hashlib,json,os,random,signal,subprocess,time
from pathlib import Path
BASE=Path(__file__).resolve().parent;ROOT=BASE.parents[1]
CASES=[('A0',1,1,'ai_first'),('S0',0,0,'fifo'),('S2',2,0,'drr_bytes'),('S3',3,0,'edf'),('S4',4,0,'importance_edf')]
random.Random(20260922).shuffle(CASES)
child=None

def stop(sig,frame):
 if child and child.poll() is None:
  child.terminate()
  try:child.wait(timeout=240)
  except subprocess.TimeoutExpired:raise RuntimeError('Child restoration did not finish; inspect board before continuing')
 raise SystemExit('Pilot interrupted; inspect last trial restoration result')
signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop)

first=BASE/'results/policy-v6-S1/outcome.json';limit=time.monotonic()+300
while json.loads(first.read_text())['status']=='running':
 if time.monotonic()>limit:raise SystemExit('S1 baseline did not finish')
 time.sleep(2)
a=json.loads(first.read_text());assert a['status']=='passed' and a['restored']
manifest=dict(kind='diagnostic_not_mission_matrix',network='unshaped',baseline_first='S1',remaining_order=[x[0] for x in CASES],replications=1,input_sha256=hashlib.sha256((ROOT/'experiments/aiot_resource_management/fixtures/replay.jpg').read_bytes()).hexdigest())
(BASE/'results/policy-pilot-v6.json').write_text(json.dumps(manifest,indent=2)+'\n')
for name,policy,fifo,policy_name in CASES:
 print(json.dumps(dict(stage='building',policy=name)),flush=True)
 env=os.environ.copy();env.update(AIOT_MOQT_CONCURRENT='ON',AIOT_MOQT_BOUNDED_PUMP='ON',AIOT_MOQT_IMAGE_SLOTS='1',AIOT_MOQT_POLICY_EXPERIMENT='ON',AIOT_MOQT_POLICY=str(policy),AIOT_MOQT_FIFO_ADMISSION=str(fifo))
 with (BASE/f'private/build-policy-{name}.log').open('w') as log:
  child=subprocess.Popen(['bash',str(BASE/'build.sh')],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT);rc=child.wait()
 if rc:raise SystemExit(f'{name}: build failed; inspect private build log')
 print(json.dumps(dict(stage='device_trial',policy=name)),flush=True)
 run=BASE/f'results/policy-v6-{name}'
 with (BASE/f'private/trial-policy-{name}.log').open('w') as log:
  child=subprocess.Popen([str(BASE/'vendor/venv/bin/python'),str(BASE/'device_trial.py'),'--dual','--replay','--policy',policy_name,'--image-slots','1','--name',run.name],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT);rc=child.wait()
 outcome=json.loads((run/'outcome.json').read_text());print(json.dumps(dict(policy=name,**outcome)),flush=True)
 if rc or not outcome.get('restored'):raise SystemExit(f'{name}: validation/restoration failed; sweep stopped')
 from analyze_policies import summary
 (run/'policy-summary.json').write_text(json.dumps(summary(run),indent=2)+'\n')
print('DONE: all six unshaped fixed-input diagnostic policies completed',flush=True)
