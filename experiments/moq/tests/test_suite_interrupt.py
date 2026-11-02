"""Regression: a signal during Popen.wait must not wait recursively on its lock."""

import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


class SuiteInterruptTests(unittest.TestCase):
    def test_interrupt_waits_for_child_cleanup_without_deadlock(self):
        module = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "results").mkdir()
            fixture = root / "fixtures/screw-ladder-v1"
            fixture.mkdir(parents=True)
            (fixture / "manifest.json").write_text('{"representations": []}')
            firmware = root / "firmware/low"
            firmware.mkdir(parents=True)
            (firmware / "firmware.bin").write_bytes(b"host-test-only")
            # A real child process models restore-on-SIGTERM, without any device.
            (root / "remote_ota_trial.py").write_text("""
import json,signal,sys,time
from pathlib import Path
root=Path(__file__).parent
name=sys.argv[sys.argv.index('--name')+1]
out=root/'results'/name;out.mkdir()
def stop(sig,frame):
    time.sleep(.05)
    (out/'manifest.json').write_text(json.dumps(dict(status='failed',restore_verified=True)))
    raise SystemExit(1)
signal.signal(signal.SIGTERM,stop)
(root/'ready').touch()
while True:time.sleep(.1)
""")
            supervisor = root / "supervisor.py"
            supervisor.write_text(f"""
import sys
from pathlib import Path
sys.path.insert(0,{str(module)!r})
import ladder_suite
ladder_suite.BASE=Path({str(root)!r})
ladder_suite.ROOT=ladder_suite.BASE
ladder_suite.PYTHON=Path(sys.executable)
sys.argv=['ladder_suite','--name','interrupt','--policies','low','--networks','128',
          '--repeats','1','--firmware-root',{str(root / "firmware")!r},'--execute']
try:ladder_suite.main()
except KeyboardInterrupt:pass
""")
            with (root / "output.log").open("w") as output:
                process = subprocess.Popen(
                    [sys.executable, supervisor], stdout=output, stderr=output
                )
                try:
                    deadline = time.monotonic() + 5
                    while not (root / "ready").exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    self.assertTrue((root / "ready").exists())
                    os.kill(process.pid, signal.SIGTERM)
                    self.assertEqual(process.wait(timeout=5), 0)
                    import json

                    manifest = json.loads((root / "results/interrupt/manifest.json").read_text())
                    self.assertEqual(manifest["status"], "stopped")
                    self.assertTrue(manifest["results"][0]["restore_verified"])
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
