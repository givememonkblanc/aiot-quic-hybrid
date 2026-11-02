import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import remote_ota_trial as trial

class RestoreStabilityTests(unittest.TestCase):
    def check(self, rollback, restore_image=None):
        with tempfile.TemporaryDirectory() as directory:
            log=Path(directory)/'service.log'
            base=(restore_image or trial.DEPLOYED).stem
            clock=[0.0]
            def tick(seconds):
                clock[0]+=seconds
                if int(clock[0])%30==0:
                    current='test' if rollback and clock[0]>=120 else base
                    with log.open('a') as f:f.write(f'current={current} HTTP 204 -\n')
            log.write_text(f'current=test HTTP 204 -\ncurrent={base} HTTP 204 -\n')
            with patch.object(trial,'SERVICE_LOG',log),patch.object(trial.time,'monotonic',side_effect=lambda:clock[0]),patch.object(trial.time,'sleep',side_effect=tick):
                result=trial.wait_for_restore(0,'test',200,restore_image)
            return result,clock[0]
    def test_waits_beyond_one_poll(self):
        result,elapsed=self.check(False)
        self.assertTrue(result)
        self.assertGreaterEqual(elapsed,150)
    def test_rollback_is_failure(self):
        self.assertFalse(self.check(True)[0])
    def test_explicit_recovery_identity(self):
        self.assertTrue(self.check(False, Path('new-recovery.bin'))[0])
