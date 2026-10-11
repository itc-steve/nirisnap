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
            "XDG_DATA_HOME": str(home / ".local/share"),
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
            self.assertIn("/usr/bin/sudo -- /usr/bin/pacman", result.stdout)
            self.assertNotIn("Would run: sudo pacman", result.stdout)
            self.assertNotIn("hypr" + "land", result.stdout.lower())

    def test_check_uses_v5_data_home_without_registry_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            registry = home / ".config/noctalia/plugins.json"
            registry.parent.mkdir(parents=True)
            registry.write_text("invalid legacy registry must not be read")
            result = self.run_check(home)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(str(home / ".local/share/noctalia/plugins/nirisnap"), result.stdout)
            self.assertEqual(registry.read_text(), "invalid legacy registry must not be read")

    def test_check_refuses_existing_plugin_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            plugin = home / ".local/share/noctalia/plugins/nirisnap"
            plugin.mkdir(parents=True)
            result = self.run_check(home)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Refusing to replace plugin path", result.stderr)


if __name__ == "__main__":
    unittest.main()
