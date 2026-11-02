"""Build the fixed replay adapter against the already restored pinned author tree."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import os
from compare_author import ROOT


def main():
    repository=ROOT.parents[1]
    source=ROOT/'upstream/mqtt-lib-lab'
    expected='8be02fb027bc034ac2c4c14dd4bd5b491a009135'
    actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
    if actual!=expected:raise RuntimeError('unexpected author source commit')
    build=ROOT/'legacy_build';build.mkdir(exist_ok=True)
    include=repository/'src/components/dl_qads/include'
    sources=[repository/'src/components/dl_qads/video_admission.c',ROOT/'fixed_replay/admission_bridge.c']
    objects=[];commands=[]
    def run(cmd,**kw):
        cmd=[str(v) for v in cmd];commands.append(cmd);subprocess.run(cmd,check=True,**kw)
    for i,path in enumerate(sources):
        obj=build/f'fixed-{i}.o';objects.append(obj)
        run(['cc','-fPIC','-O2','-Wall','-Wextra','-Werror','-I',include,'-c',path,'-o',obj])
    (build/'libreplay_admission.a').unlink(missing_ok=True)
    run(['ar','rcs',build/'libreplay_admission.a',*objects])
    run(['cc','-shared',*objects,'-o',build/'libreplay_admission.so'])
    example=source/'crates/mqttv5-cli/examples/fixed_replay.rs';example.parent.mkdir(exist_ok=True)
    shutil.copyfile(ROOT/'fixed_replay/client.rs',example)
    target=ROOT/'upstream/mqtt-lib/target'
    env=dict(os.environ,CARGO_TARGET_DIR=str(target))
    run(['cargo','rustc','--release','--locked','--offline','--manifest-path',source/'Cargo.toml',
         '-p','mqttv5-cli','--example','fixed_replay','-j','4','--','-L',f'native={build}','-l','static=replay_admission'],env=env)
    shutil.copyfile(target/'release/examples/fixed_replay',build/'fixed-replay');(build/'fixed-replay').chmod(0o755)
    files=[*sources,include/'video_admission.h',ROOT/'fixed_replay/client.rs',source/'Cargo.lock',build/'fixed-replay',build/'libreplay_admission.so']
    manifest=dict(author_commit=actual,commands=commands,
        rustc=subprocess.check_output(['rustc','--version'],text=True).strip(),
        cc=subprocess.check_output(['cc','--version'],text=True).splitlines()[0],
        sha256={str(p.relative_to(repository)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files})
    (ROOT/'fixed_build_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
