"""Offline v5 manifest checks and executable launcher regressions; no captures."""
import json
import os
import shutil
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = ROOT / "plugin/nirisnap"


class PluginContractTest(unittest.TestCase):
    def test_manifest_and_translations(self):
        manifest = tomllib.loads((PLUGIN / "plugin.toml").read_text())
        self.assertEqual(manifest["id"], "itc-steve/nirisnap")
        self.assertEqual(manifest["plugin_api"], 24)
        self.assertEqual(manifest["widget"][0]["actions"]["middle"], "none")
        strings = json.loads((PLUGIN / "translations/en.json").read_text())
        for setting in manifest["setting"]:
            self.assertIn(setting["label_key"], strings)
            for option in setting.get("options", []):
                self.assertIn(option["label_key"], strings)
        for kind in ("widget", "panel"):
            self.assertTrue((PLUGIN / manifest[kind][0]["entry"]).is_file())
        self.assertFalse(list(PLUGIN.rglob("*.qml")))
        self.assertFalse((PLUGIN / "manifest.json").exists())

    def test_actual_luau_logic_with_stub_host(self):
        # Scripts use the Lua-compatible subset; execute real source, not a mirror.
        lua = shutil.which("lua")
        self.assertIsNotNone(lua, "Install lua to run offline plugin regressions")
        result = subprocess.run([lua, str(ROOT / "tests/plugin-regression.lua"), str(PLUGIN)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("plugin regressions passed", result.stdout)

    def test_delayed_helper_preserves_argv_and_rejects_options(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            path = Path(directory)
            output = path / "argv"
            fake = path / "nirisnap"
            fake.write_text('#!/bin/sh\n[ "$(cat "$TEST_ARGV.delay")" = "0.2" ] || exit 1\nprintf "%s\\n" "$@" > "$TEST_ARGV"\n')
            fake.chmod(0o755)
            sleep = path / "sleep"
            sleep.write_text('#!/bin/sh\nprintf "%s" "$1" > "$TEST_ARGV.delay"\n')
            sleep.chmod(0o755)
            env = os.environ | {"PATH": str(path) + ":" + os.environ["PATH"], "TEST_ARGV": str(output)}
            result = subprocess.run(["/bin/sh", str(PLUGIN / "launch.sh"),
                                     "--capture-region", "--copy", "--save"], env=env)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(output.read_text().splitlines(), ["--capture-region", "--copy", "--save"])
            output.unlink()
            for args in (["--file", "/tmp/nope"], ["--capture-region;touch x"],
                         ["--capture-region", "--evil"], ["--clipboard", "--save"]):
                result = subprocess.run(["/bin/sh", str(PLUGIN / "launch.sh"), *args], env=env,
                                        capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
