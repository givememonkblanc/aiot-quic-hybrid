import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('pack_model',ROOT/'src/components/ai_inference/tools/pack_model.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class PackModelTest(unittest.TestCase):
    def test_exact_weights_and_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp);source=ROOT/'src/components/ai_inference/ai_inference_vehicle_model.c'
            metadata=ROOT/'src/components/ai_inference/include/ai_inference_model_asset_generated.h'
            module.pack(source,metadata,out)
            def array(path):
                body=path.read_text().split('{',1)[1].split('}',1)[0]
                return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})\b',body))
            registrations=(out/'ai_model_register_ops.inc').read_text()
            for op in ('AddPad()', 'AddAdd()', 'AddMean()', 'AddFullyConnected()', 'AddConv2D()'):
                self.assertIn(op,registrations)
            self.assertEqual(registrations.count('if ('),8)
            self.assertEqual(zlib.decompress(array(out/'ai_model_compact.c')),array(source))
            self.assertEqual((out/'ai_model_compact.h').read_text(),metadata.read_text().replace('.data = g_vehicle_fomo_model_data,','.data = NULL,'))

if __name__=='__main__':unittest.main()
