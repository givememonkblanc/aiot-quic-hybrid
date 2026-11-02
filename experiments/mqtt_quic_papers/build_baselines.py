"""Restore pinned author implementations locally; never install system packages."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parent
URLS={
    'paho':'https://github.com/pgOrtiz90/paho.mqtt.golang.git',
    'volantmq':'https://github.com/fatimafp95/volantmq.git',
    'quic-go-fernandez':'https://github.com/fatimafp95/quic-go.git',
    'mqtt-lib':'https://github.com/LabOverWire/mqtt-lib.git',
}


def run(command,cwd):
    subprocess.run([str(x) for x in command],cwd=cwd,check=True)


def source(name,commit):
    path=ROOT/'upstream'/name
    if not path.exists():
        run(['git','clone','--no-checkout',URLS[name],path],ROOT)
        run(['git','checkout','--detach',commit],path)
    actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=path,text=True).strip()
    if actual!=commit:
        raise RuntimeError(f'{path}: existing checkout differs; refusing to reset it')
    return path


def old_go():
    root=ROOT/'upstream/toolchains';root.mkdir(exist_ok=True)
    binary=root/'go1.14.15/go/bin/go'
    if binary.exists():
        return binary
    releases=json.load(urllib.request.urlopen('https://go.dev/dl/?mode=json&include=all',timeout=30))
    release=next(r for r in releases if r['version']=='go1.14.15')
    item=next(f for f in release['files'] if f['os']=='linux' and f['arch']=='amd64' and f['kind']=='archive')
    archive=root/item['filename']
    with urllib.request.urlopen('https://go.dev/dl/'+item['filename'],timeout=60) as response, archive.open('wb') as out:
        shutil.copyfileobj(response,out)
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=item['sha256']:
        raise ValueError('Go archive checksum mismatch')
    with tarfile.open(archive) as tar:
        tar.extractall(root/'go1.14.15',filter='data')
    return binary


def lab_copy(name):
    src=ROOT/'upstream'/name;dst=ROOT/'upstream'/(name+'-lab')
    if not dst.exists():
        shutil.copytree(src,dst,ignore=shutil.ignore_patterns('.git'))
        run(['patch','-p1','-i',ROOT/'patches'/(name+'-lab.patch')],dst)
    # Never modify the immutable original checkout's module file.
    module=dst/'go.mod'
    module.write_text(re.sub(r'^replace github.com/lucas-clemente/quic-go v0.15.1 => .*$',
        f'replace github.com/lucas-clemente/quic-go v0.15.1 => {ROOT}/upstream/quic-go-fernandez',
        module.read_text(),flags=re.M))
    return dst


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--family',choices=['legacy','modern','all'],default='all')
    args=parser.parse_args()
    manifest=json.loads((ROOT/'baseline_build_manifest.json').read_text())
    commits={r['name']:r['commit'] for r in manifest['repositories']}
    (ROOT/'upstream').mkdir(exist_ok=True)
    if args.family in ('legacy','all'):
        for name in ('paho','volantmq','quic-go-fernandez'):
            source(name,commits[name])
        binary=old_go()
        output=ROOT/'legacy_build';output.mkdir(exist_ok=True)
        run([binary,'build','-o',output/'legacy-client',ROOT/'legacy_client.go'],lab_copy('paho'))
        run([binary,'build','-o',output/'volantmq-lab','./cmd/volantmq'],lab_copy('volantmq'))
    if args.family in ('modern','all'):
        src=source('mqtt-lib',commits['mqtt-lib'])
        lock=src/'Cargo.lock'
        if lock.exists() and lock.read_bytes()!=(ROOT/'author-Cargo.lock').read_bytes():
            raise RuntimeError('Existing Cargo.lock differs; refusing to overwrite')
        shutil.copy(ROOT/'author-Cargo.lock',lock)
        run(['cargo','build','--locked','--release','-p','mqttv5-cli','-j','4'],src)
        output=ROOT/'legacy_build';output.mkdir(exist_ok=True)
        shutil.copy(src/'target/release/mqttv5',output/'mqttv5-author')
        lab=ROOT/'upstream/mqtt-lib-lab'
        if not lab.exists():
            run(['git','worktree','add','--detach',lab,commits['mqtt-lib']],src)
            run(['patch','-p1','-i',ROOT/'patches/mqtt-lib-measurement.patch'],lab)
        shutil.copy(ROOT/'author-Cargo.lock',lab/'Cargo.lock')
        env=dict(os.environ,CARGO_TARGET_DIR=str(src/'target'))
        subprocess.run(['cargo','build','--locked','--release','-p','mqttv5-cli','-j','4'],
                       cwd=lab,env=env,check=True)
        shutil.copy(src/'target/release/mqttv5',output/'mqttv5-lab')


if __name__=='__main__':
    main()
