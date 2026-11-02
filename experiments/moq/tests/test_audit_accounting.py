import json, struct, tempfile, unittest, zlib
from pathlib import Path
from validate_audit import validate


class AccountingTests(unittest.TestCase):
    def fixture(self, p, duplicate=False):
        lines = []

        def add(tag, **v):
            lines.append(tag + " " + json.dumps(v))

        for seq in (1, 2):
            add("[MOQ_CAPTURE]", seq=seq)
            add("[REPLAY]", seq=seq)
        lines.append("[MOQ_CAPTURE_END] count=2 failed=0")
        for t in ("[AI_START]", "[AI_PROFILE]", "[ESPDET_PHASE]"):
            add(t, seq=1)
        add("[AI_OUTCOME]", seq=1, success=1)
        add("[MOQ_AI_PRODUCED]", seq=1, enqueued=1)
        add("[MOQ_PENDING]", alias=1, group=2)
        if duplicate:
            add("[MOQ_PENDING]", alias=1, group=1)
        b = b"".join(struct.pack("<H", len(x.encode())) + x.encode() for x in lines)
        (p / "audit.bin").write_bytes(b)
        (p / "audit-manifest.json").write_text(
            json.dumps(
                dict(
                    bytes=len(b),
                    records=len(lines),
                    crc=zlib.crc32(b),
                    overflow=0,
                    truncated=0,
                    late=0,
                )
            )
        )
        (p / "device-events.json").write_text(json.dumps([dict(line=x, host=None) for x in lines]))
        (p / "events.jsonl").write_text(
            "\n".join(json.dumps(dict(event="object", alias=a, group=1)) for a in (1, 2))
        )

    def test_every_source_has_one_terminal_state(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            self.fixture(p)
            r = validate(p)
            self.assertEqual(r["source_count"], 2)
            self.assertEqual(r["terminal_counts"]["1"], {"received": 1, "pending_at_cutoff": 1})

    def test_duplicate_terminal_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            self.fixture(p, True)
            with self.assertRaisesRegex(AssertionError, "duplicate terminal"):
                validate(p)

    def test_derived_json_cannot_hide_record(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            self.fixture(p)
            r = json.loads((p / "device-events.json").read_text())
            r.pop()
            (p / "device-events.json").write_text(json.dumps(r))
            with self.assertRaisesRegex(AssertionError, "differ"):
                validate(p)


if __name__ == "__main__":
    unittest.main()
