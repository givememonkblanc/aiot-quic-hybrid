import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import remote_ota_trial as trial


class RestoreTests(unittest.TestCase):
    def check(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "log"
            log.write_text(text)
            with (
                patch.object(trial, "SERVICE_LOG", log),
                patch.object(trial.time, "sleep"),
                patch.object(trial.time, "monotonic", side_effect=range(20)),
            ):
                return trial.wait_for_restore(0, "testhash", 1)

    def test_requires_baseline_204_on_same_line(self):
        baseline = trial.DEPLOYED.stem
        self.assertFalse(
            self.check(
                f"current=testhash 204 -\ncurrent={baseline} 200 -\ncurrent=testhash 204 -\n"
            )
        )

    def test_baseline_before_test_does_not_count(self):
        self.assertFalse(
            self.check(f"current={trial.DEPLOYED.stem} 204 -\ncurrent=testhash 204 -\n")
        )

    def test_single_fresh_baseline_confirmation_is_insufficient(self):
        self.assertFalse(
            self.check(f"current=testhash 204 -\ncurrent={trial.DEPLOYED.stem} 204 -\n")
        )


if __name__ == "__main__":
    unittest.main()
