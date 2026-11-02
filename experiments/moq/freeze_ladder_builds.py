"""Archive shared-source provenance for the bounded v17 screen.
Variant overrides are explicit, not represented as recovered compiler logs.
"""
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'experiments/moq/results/ladder-v17-firmware'
files=['src/main/app_moq_concurrent.inc','src/main/CMakeLists.txt',
       'src/components/transport/moqt_feasibility.h','src/components/transport/moqt_ngtcp2.inc',
       'src/components/transport/moqt_scheduler.c','src/components/transport/moqt_tx_policy.h',
       'src/components/sources/source_manager.c','src/components/sources/source_async.inc',
       'experiments/screw_mission/out/ai_inference_vehicle_model.c',
       'experiments/screw_mission/out/screw_model_asset.h',
       'experiments/moq/fixtures/screw-ladder-v1/manifest.json']
provenance=dict(schema=1,scope='shared source hashes and explicit variant overrides',files={},variants={})
for name in files:
    provenance['files'][name]=hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
config={}
for line in (ROOT/'build_moq_screw_cap24/CMakeCache.txt').read_text().splitlines():
    if line.startswith('AIOT_') and '=' in line:
        key,value=line.split('=',1);config[key.split(':',1)[0]]=value.replace(str(ROOT),'${ROOT}')
# Final common cache is retained as such; its mode is NOT attributed to all binaries.
provenance['common_cache_with_last_variant']=config
for name,rep,feasible,timeout in [('high',0,0,0),('high_timeout',0,0,1),('low',1,0,1),('medium',3,0,1),('adaptive',2,1,1)]:
    data=(OUT/name/'firmware.bin').read_bytes()
    provenance['variants'][name]=dict(image_sha256=hashlib.sha256(data).hexdigest(),size=len(data),
        overrides=dict(AIOT_MOQT_REPRESENTATION=rep,AIOT_MOQT_FEASIBILITY=feasible,
                       AIOT_MOQT_DELIVERY_TIMEOUT_RESET=timeout,PROJECT_VER=f'moqt-ladder-{name}-v17'))
(OUT/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
print(OUT/'provenance.json')
