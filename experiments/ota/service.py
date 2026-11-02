#!/usr/bin/env python3
"""Local-only release publication and HTTPS pull service for the research ESP32."""
import argparse
import hashlib
import http.server
import ipaddress
import json
import os
from pathlib import Path
import re
import ssl
import struct
import subprocess
import tempfile
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT/'experiments/ota/private'
HEX = re.compile(r'^[0-9a-f]{64}$')

def image_identity(data):
    if len(data) < 288 or data[0] != 0xe9 or struct.unpack_from('<I', data, 32)[0] != 0xabcd5432:
        raise ValueError('Not an ESP app image with an app descriptor')
    if len(data) > 0x1f0000: raise ValueError('Image exceeds OTA slot')
    if struct.unpack_from('<H', data, 12)[0] != 9: raise ValueError('Expected ESP32-S3 chip ID')
    return data[176:208].hex()

def atomic_write(path, data):
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as f: f.write(data); f.flush(); os.fsync(f.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)

def init_site(directory, host, port):
    ipaddress.ip_address(host)
    directory.mkdir(parents=True, exist_ok=True); directory.chmod(0o700)
    key, cert = directory/'server.key', directory/'server.pem'
    if not key.exists() and not cert.exists():
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-sha256',
            '-days','3650','-keyout',str(key),'-out',str(cert),'-subj','/CN='+host,
            '-addext','subjectAltName=IP:'+host,'-addext','basicConstraints=critical,CA:TRUE'],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        key.chmod(0o600)
    elif not key.exists() or not cert.exists(): raise ValueError('Incomplete TLS identity; restore the matching key/certificate')
    header = '#pragma once\n#define AIOT_OTA_BASE_URL '+json.dumps(f'https://{host}:{port}')+'\n'
    header += 'static const char aiot_ota_server_cert[] =\n'
    header += '\n'.join(json.dumps(line+'\n') for line in cert.read_text().splitlines())+';\n'
    (directory/'ota_site.h').write_text(header)
    (directory/'site.json').write_text(json.dumps({'host':host,'port':port})+'\n')
    print('Site ready:',directory, '(keep server.key private and retain this TLS identity)')

def publish(directory, image):
    data = image.read_bytes(); identity = image_identity(data)
    images=directory/'images';images.mkdir(exist_ok=True)
    destination=images/(identity+'.bin')
    if destination.exists() and destination.read_bytes()!=data: raise ValueError('ELF identity reused with a different binary')
    atomic_write(destination,data)
    release={'elf_sha256':identity,'image_sha256':hashlib.sha256(data).hexdigest(),'size':len(data)}
    atomic_write(directory/'release.json',(json.dumps(release,indent=2)+'\n').encode())
    print(json.dumps(release))

class TLSServer(http.server.ThreadingHTTPServer):
    """A stalled TLS peer must not block accepting all later OTA polls."""
    daemon_threads = True

    def process_request_thread(self, request, client_address):
        try:
            request.settimeout(10)
            request = self.tls.wrap_socket(request, server_side=True)
            super().process_request_thread(request, client_address)
        except (OSError, ssl.SSLError):
            request.close()

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parts=urlparse(self.path); directory=self.server.directory
        if parts.path=='/next':
            current=parse_qs(parts.query).get('current',[''])[0]
            # Initial bootstrap used IDF's 9-character display hash.
            # New firmware sends all 64 characters from the app descriptor.
            if not (HEX.fullmatch(current) or re.fullmatch(r'[0-9a-f]{9}',current)):
                self.send_error(400);return
            release=directory/'release.json'
            target=json.loads(release.read_text())['elf_sha256'] if release.exists() else current
            if target.startswith(current):
                self.send_response(204);self.end_headers();return
            body=target.encode(); self.send_response(200)
            self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        elif parts.path.startswith('/images/') and parts.path.endswith('.bin'):
            identity=parts.path[len('/images/'):-4]
            if not HEX.fullmatch(identity): self.send_error(404);return
            image=directory/'images'/(identity+'.bin')
            if not image.exists():self.send_error(404);return
            self.send_response(200);self.send_header('Content-Type','application/octet-stream')
            self.send_header('Content-Length',str(image.stat().st_size));self.end_headers()
            with image.open('rb') as f:
                while chunk:=f.read(16384):self.wfile.write(chunk)
        else:self.send_error(404)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directory',type=Path,default=DEFAULT)
    sub=p.add_subparsers(dest='command',required=True)
    init=sub.add_parser('init');init.add_argument('--host',required=True);init.add_argument('--port',type=int,default=18443)
    pub=sub.add_parser('publish');pub.add_argument('image',type=Path)
    sub.add_parser('serve');sub.add_parser('unpublish')
    args=p.parse_args();directory=args.directory.resolve()
    if args.command=='init':init_site(directory,args.host,args.port)
    elif args.command=='publish':publish(directory,args.image)
    elif args.command=='unpublish':(directory/'release.json').unlink(missing_ok=True)
    else:
        site=json.loads((directory/'site.json').read_text())
        server=TLSServer(('0.0.0.0',site['port']),Handler);server.directory=directory
        tls=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);tls.minimum_version=ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(directory/'server.pem',directory/'server.key');server.tls=tls
        print('HTTPS OTA pull service on port',site['port'],flush=True);server.serve_forever()

if __name__=='__main__':main()
