import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "packaging" / "install-local.sh"


class InstallerTest(unittest.TestCase):
    def run_check(self, home: Path):
        env = os.environ | {
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "XDG_CACHE_HOME": str(home / ".cache"),
            "NIRISNAP_PREFIX": str(home / ".local"),
        }
        return subprocess.run(
            [INSTALLER, "--check"], env=env, text=True, capture_output=True
        )

    def test_check_is_read_only(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            before = sorted(home.rglob("*"))
            result = self.run_check(home)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(sorted(home.rglob("*")), before)
            self.assertIn("Preflight passed", result.stdout)
            self.assertIn("layer-shell-qt", result.stdout)
            self.assertNotIn("hypr" + "land", result.stdout.lower())

    def test_check_refuses_existing_plugin_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            plugin = home / ".config/noctalia/plugins/nirisnap"
            plugin.mkdir(parents=True)
            result = self.run_check(home)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Refusing to replace plugin path", result.stderr)


if __name__ == "__main__":
    unittest.main()
