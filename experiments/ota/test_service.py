import json
from pathlib import Path
import struct
import tempfile
import unittest
import socket
import ssl
import threading
import urllib.request
from service import image_identity, publish, init_site, TLSServer, Handler


def fixture():
    image=bytearray(1024);image[0]=0xe9
    struct.pack_into('<H',image,12,9);struct.pack_into('<I',image,32,0xabcd5432)
    image[176:208]=bytes(range(32))
    return image

class ServiceTest(unittest.TestCase):
    def test_stalled_tls_peer_does_not_block_next_poll(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            init_site(root, '127.0.0.1', 0)
            server = TLSServer(('127.0.0.1', 0), Handler)
            server.directory = root
            server.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            server.tls.load_cert_chain(root/'server.pem', root/'server.key')
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            stalled = socket.create_connection(server.server_address)
            try:
                context = ssl.create_default_context(cafile=str(root/'server.pem'))
                url = 'https://127.0.0.1:%d/next?current=%s' % (server.server_port, 'a'*64)
                with urllib.request.urlopen(url, context=context, timeout=2) as response:
                    self.assertEqual(response.status, 204)
            finally:
                stalled.close()
                server.shutdown(); server.server_close(); thread.join()

    def test_image_identity_and_slot_boundary(self):
        data=fixture()
        self.assertEqual(image_identity(data),bytes(range(32)).hex())
        for invalid in [b'not firmware',data[:100],data+bytes(0x1f0000)]:
            with self.assertRaises(ValueError):image_identity(invalid)
        data[12]=0
        with self.assertRaises(ValueError):image_identity(data)

    def test_atomic_release_and_immutable_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'input.bin';data=fixture();source.write_bytes(data)
            publish(root,source)
            release=json.loads((root/'release.json').read_text())
            self.assertEqual(release['size'],len(data))
            self.assertEqual((root/'images'/(release['elf_sha256']+'.bin')).read_bytes(),data)
            data[-1]=1;source.write_bytes(data)
            with self.assertRaises(ValueError):publish(root,source)
            self.assertEqual(json.loads((root/'release.json').read_text()),release)

if __name__=='__main__':unittest.main()
