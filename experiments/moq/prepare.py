"""Prepare local test credentials and SDK config without overwriting existing ones."""
import hashlib,subprocess,os
from pathlib import Path
base=Path(__file__).resolve().parent;root=base.parents[1]
private=base/'private';private.mkdir(exist_ok=True,mode=0o700);os.chmod(private,0o700)
cert=private/'cert.pem';key=private/'key.pem'
if cert.exists()!=key.exists():raise SystemExit('Incomplete cert/key pair; inspect private directory')
if not cert.exists():
 subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','7','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
os.chmod(key,0o600)
subprocess.run(['openssl','x509','-checkend','0','-noout','-in',str(cert)],check=True,stdout=subprocess.DEVNULL)
der=subprocess.check_output(['openssl','x509','-in',str(cert),'-outform','DER'])
pin=hashlib.sha256(der).digest()
(private/'pin.h').write_text('static const unsigned char moqt_peer_pin[32] = {'+','.join(map(str,pin))+'};\n')
config=private/'sdkconfig'
if not config.exists():config.write_bytes((root/'experiments/vlm_split/integrated/sdkconfig').read_bytes())
(base/'results').mkdir(exist_ok=True)
print('Prepared local certificate pin and isolated config; rebuild after changing certificate.')
