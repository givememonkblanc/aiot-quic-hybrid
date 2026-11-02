import json
import tempfile
import unittest
from pathlib import Path
from analyze import analyze


class AnalyzeTest(unittest.TestCase):
    def test_requires_delivery_and_no_reconnect(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            lines = ['event=research_start', 'event=session_ready session=1 t_us=1000000',
                     'event=generate seq=1 critical=1 queued=1 t_us=1500000',
                     'event=link_down session=1 t_us=2000000',
                     'event=link_up session=1 t_us=3000000',
                     'event=path_validation session=1 result=2 elapsed_ms=100 t_us=3100000',
                     'event=generate seq=2 critical=1 queued=1 t_us=3200000',
                     'event=metrics t_us=6000000']
            def write():
                (p/'serial.log').write_text('\n'.join('MOBILITY: '+line for line in lines))
            write()
            rows = [dict(t=t, cid='same', payload=json.dumps(dict(seq=seq, gen_us=gen, critical=True)))
                    for seq, gen, t in [(1,1500000,9000000),(2,3200000,10700000)]]
            (p/'feed.jsonl').write_text('\n'.join(map(json.dumps,rows)))
            result = analyze(p)
            self.assertTrue(result['handoffs'][0]['session_preserved'])
            self.assertEqual(result['handoffs'][0]['wifi_outage_ms'], 1000)
            self.assertEqual(result['delivery_after_first_ready']['critical']['ratio'], 1)
            lines.append('event=session_begin session=2 t_us=6100000')
            write()
            self.assertFalse(analyze(p)['handoffs'][0]['session_preserved'])
            # A truncated serial line must not crash or count as a generated sample.
            lines.append('event=generate seq=3 critical=1 queued=1')
            write()
            self.assertEqual(analyze(p)['delivery_after_first_ready']['critical']['generated'], 2)

if __name__ == '__main__': unittest.main()
