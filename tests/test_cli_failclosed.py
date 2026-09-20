"""CLI fail-closed checks for empty --file/--pin."""

import os
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build" / "nirisnap"


@unittest.skipUnless(BINARY.is_file(), "nirisnap not built")
class FailClosedCli(unittest.TestCase):
    def run_cli(self, *args):
        env = os.environ | {"QT_QPA_PLATFORM": "offscreen"}
        return subprocess.run(
            [str(BINARY), *args],
            env=env,
            text=True,
            capture_output=True,
        )

    def test_empty_file_option_is_usage_error(self):
        result = self.run_cli("--file=", "--capture-fullscreen", "--copy")
        self.assertEqual(result.returncode, 2, result.stderr)

    def test_empty_pin_option_is_usage_error(self):
        result = self.run_cli("--pin=")
        self.assertEqual(result.returncode, 2, result.stderr)


if __name__ == "__main__":
    unittest.main()
