import asyncio
from functools import partial
from pathlib import Path
import tempfile
import unittest
from aioquic.asyncio import connect, serve
from aioquic.quic.configuration import QuicConfiguration
from aioquic.h3.connection import H3_ALPN
from compare_author import certs, free_port
from h3_pubsub import Broker, Client, basic, client_config


class H3ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='h3-functional-')
        self.root=Path(self.temp.name);certs(self.root)
        config=QuicConfiguration(is_client=False,alpn_protocols=H3_ALPN)
        config.load_cert_chain(self.root/'server.pem',self.root/'server.key')
        self.port=free_port(True)
        self.server=await serve('127.0.0.1',self.port,configuration=config,
            create_protocol=partial(Broker,topics={},credential=basic('test:ephemeral')))

    async def asyncTearDown(self):
        self.server.close();await asyncio.sleep(.05);self.temp.cleanup()

    def connection(self,password='test:ephemeral'):
        return connect('127.0.0.1',self.port,configuration=client_config(self.root/'ca.pem'),
                       create_protocol=partial(Client,credential=basic(password)))

    async def test_mapping_auth_cache_and_delivery(self):
        async with self.connection() as pub, self.connection() as sub:
            self.assertEqual(await pub.request('HEAD','missing'),404)
            self.assertEqual(await sub.request('GET','missing',callback=lambda _:None),404)
            self.assertFalse(sub.callbacks)
            self.assertEqual(await pub.request('POST','missing',b'data'),404)
            self.assertEqual(await pub.request('PUT','one'),201)
            self.assertTrue(pub.authenticated)
            pub.credential=None  # subsequent requests must use server's connection auth cache
            self.assertEqual(await pub.request('HEAD','one'),200)
            messages=[]
            self.assertEqual(await sub.request('GET','one',callback=messages.append),200)
            data=bytes(range(256))*256
            self.assertEqual(await pub.request('POST','one',data),200)
            for _ in range(200):
                if messages:break
                await asyncio.sleep(.01)
            self.assertEqual(messages,[data])
            self.assertEqual(await pub.request('DELETE','one'),204)
            self.assertEqual(await pub.request('HEAD','one'),404)
            self.assertEqual(await pub.request('PATCH','one'),405)

    async def test_wrong_credentials_rejected(self):
        async with self.connection('wrong:value') as client:
            self.assertEqual(await client.request('PUT','one'),401)
            self.assertFalse(client.authenticated)

    async def test_wrong_ca_rejected(self):
        other=self.root/'other';other.mkdir();certs(other)
        config=client_config(other/'ca.pem');config.idle_timeout=.5
        with self.assertRaises(ConnectionError):
            async with connect('127.0.0.1',self.port,configuration=config,create_protocol=Client):
                self.fail('untrusted server accepted')


if __name__=='__main__':unittest.main()
