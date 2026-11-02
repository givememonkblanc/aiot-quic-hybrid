import json, struct, unittest, zlib
from audit_reader import AuditReader


class JournalTests(unittest.TestCase):
    def fixture(self):
        lines = ["[MOQ_CAPTURE] " + ("x" * 350) + "\n", "[MOQ_DONE] subscriber_finished=1\n"]
        b = b"".join(struct.pack("<H", len(x)) + x.encode() for x in lines)
        head = dict(
            schema=1,
            capacity=384 * 1024,
            bytes=len(b),
            records=len(lines),
            pages=(len(b) + 255) // 256,
            crc=zlib.crc32(b),
            overflow=0,
            truncated=0,
            late=0,
        )
        pages = [
            dict(
                i=i,
                o=i * 256,
                n=len(b[i * 256 : (i + 1) * 256]),
                crc=zlib.crc32(b[i * 256 : (i + 1) * 256]),
                hex=b[i * 256 : (i + 1) * 256].hex(),
            )
            for i in range(head["pages"])
        ]
        return b, head, pages, dict(bytes=len(b), crc=zlib.crc32(b), late=0)

    def send(self, r, t, d):
        r.consume("[" + t + "] " + json.dumps(d))

    def test_retry_and_exact_reconstruction(self):
        b, h, p, e = self.fixture()
        sent = []
        r = AuditReader(sent.append, True)
        self.send(r, "AUDIT_BEGIN", h)
        self.send(r, "AUDIT_CHUNK", p[0])
        self.assertEqual(sent, [b"A -1\n"])
        self.send(r, "AUDIT_CHUNK", p[0])
        self.assertEqual(r.duplicates, 1)
        for x in p[1:]:
            self.send(r, "AUDIT_CHUNK", x)
        self.send(r, "AUDIT_END", e)
        self.assertTrue(r.complete)
        self.assertEqual(r.raw, b)

    def test_corrupt_missing_page_never_completes(self):
        b, h, p, e = self.fixture()
        r = AuditReader(lambda _: None)
        self.send(r, "AUDIT_BEGIN", h)
        bad = dict(p[0], crc=p[0]["crc"] ^ 1)
        self.send(r, "AUDIT_CHUNK", bad)
        for x in p[1:]:
            self.send(r, "AUDIT_CHUNK", x)
        self.send(r, "AUDIT_END", e)
        self.assertFalse(r.complete)

    def test_overflow_and_invalid_total(self):
        b, h, p, e = self.fixture()
        r = AuditReader(lambda _: None)
        self.send(r, "AUDIT_BEGIN", dict(h, overflow=1))
        self.assertIsNone(r.begin)
        self.send(r, "AUDIT_BEGIN", h)
        for x in p:
            self.send(r, "AUDIT_CHUNK", x)
        self.send(r, "AUDIT_END", dict(e, crc=e["crc"] ^ 1))
        self.assertFalse(r.complete)

    def test_changed_duplicate_rejected(self):
        b, h, p, e = self.fixture()
        r = AuditReader(lambda _: None)
        self.send(r, "AUDIT_BEGIN", h)
        self.send(r, "AUDIT_CHUNK", p[0])
        q = bytearray.fromhex(p[0]["hex"])
        q[4] ^= 1
        self.send(r, "AUDIT_CHUNK", dict(p[0], hex=q.hex(), crc=zlib.crc32(q)))
        self.assertIn("changed", r.last_error)


if __name__ == "__main__":
    unittest.main()
