"""Static contract checks for the nirisnap Noctalia plugin.

Stdlib-only. Verifies the manifest/settings/i18n shapes, the fixed argv
allowlist in Main.qml, the IPC surface, and the panel-close-before-capture
delay, without loading QML or launching anything.
"""

import json
import re
import unittest
from pathlib import Path

PLUGIN_DIR = Path(__file__).resolve().parent.parent / "plugin" / "nirisnap"

MODES = ["region", "window", "fullscreen"]
OUTPUTS = ["edit", "copy", "save", "copy-save"]
DEFAULT_SETTINGS = {"defaultMode": "region", "outputMode": "edit", "iconColor": "none"}
ENTRY_POINTS = {
    "main": "Main.qml",
    "barWidget": "BarWidget.qml",
    "panel": "Panel.qml",
    "settings": "Settings.qml",
}
I18N_FILES = ["i18n/en.json"]
ALL_FILES = ["manifest.json", "settings.json", "Main.qml", "BarWidget.qml",
             "Panel.qml", "Settings.qml", "i18n/en.json"]


def read(name):
    return (PLUGIN_DIR / name).read_text(encoding="utf-8")


def expected_argv(mode, output):
    """Mirror of Main.qml buildArgs; the contract argv table."""
    assert mode in MODES and output in OUTPUTS
    args = ["nirisnap", "--capture-" + mode]
    if output in ("copy", "copy-save"):
        args.append("--copy")
    if output in ("save", "copy-save"):
        args.append("--save")
    return args


def extract_i18n_keys(obj, prefix=""):
    keys = set()
    for key, value in obj.items():
        full = f"{prefix}{key}"
        if isinstance(value, dict):
            keys.update(extract_i18n_keys(value, full + "."))
        else:
            keys.add(full)
    return keys


def ipc_block(source):
    start = source.index("IpcHandler {")
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError("unbalanced IpcHandler block")


class TestManifest(unittest.TestCase):
    def test_all_seven_files_exist(self):
        for name in ALL_FILES:
            self.assertTrue((PLUGIN_DIR / name).is_file(), f"missing {name}")

    def test_manifest_contract(self):
        manifest = json.loads(read("manifest.json"))
        self.assertEqual(manifest["id"], "nirisnap")
        self.assertEqual(manifest["version"], "0.3.0")
        self.assertEqual(manifest["minNoctaliaVersion"], "4.4.3")
        self.assertEqual(manifest["entryPoints"], ENTRY_POINTS)
        self.assertEqual(manifest["dependencies"]["plugins"], [])
        for entry in ENTRY_POINTS.values():
            self.assertTrue((PLUGIN_DIR / entry).is_file(), f"missing entry point {entry}")

    def test_default_settings_shape(self):
        manifest = json.loads(read("manifest.json"))
        self.assertEqual(manifest["metadata"]["defaultSettings"], DEFAULT_SETTINGS)

    def test_settings_json_matches_defaults(self):
        settings = json.loads(read("settings.json"))
        self.assertEqual(settings, DEFAULT_SETTINGS)


class TestI18n(unittest.TestCase):
    def test_en_json_valid(self):
        for name in I18N_FILES:
            self.assertIsInstance(json.loads(read(name)), dict)

    def test_every_tr_key_resolves(self):
        keys = extract_i18n_keys(json.loads(read("i18n/en.json")))
        for qml in ["Main.qml", "BarWidget.qml", "Panel.qml", "Settings.qml"]:
            # Static keys only: the literal must be the complete key, closed
            # by "," or ")" (dynamic prefixes like tr("mode." + ...) are
            # covered by test_dynamic_enum_keys_covered).
            for key in re.findall(r'tr\("([^"]+)"\s*[,)]', read(qml)):
                self.assertIn(key, keys, f"{qml} translates missing key {key}")

    def test_dynamic_enum_keys_covered(self):
        keys = extract_i18n_keys(json.loads(read("i18n/en.json")))
        for mode in MODES:
            self.assertIn(f"mode.{mode}", keys)
        for output in OUTPUTS:
            self.assertIn(f"output.{output}", keys)


class TestMainContract(unittest.TestCase):
    def setUp(self):
        self.main = read("Main.qml")

    def test_build_args_signature(self):
        self.assertIn("import Quickshell.Io", self.main)
        # Pure function tested separately in tests/tst_plugin.qml.
        self.assertRegex(self.main, r"function\s+buildArgs\s*\(\s*mode\s*,\s*output\s*\)")
        self.assertIn('"nirisnap", "--capture-" + root.validMode(mode)', self.main)

    def test_argv_allowlist_only(self):
        # Every argv element ever produced: fixed binary + fixed flags. The
        # mode flag is built as "--capture-" + validated mode.
        argv_strings = set(re.findall(r'"((?:nirisnap|--[a-z][a-z-]*))"', self.main))
        self.assertEqual(argv_strings,
                         {"nirisnap", "--capture-", "--copy", "--save", "--clipboard"})
        for arg in argv_strings:
            self.assertNotIn(" ", arg)

    def test_output_flag_mapping(self):
        self.assertRegex(self.main, r'o === "copy" \|\| o === "copy-save"')
        self.assertRegex(self.main, r'args\.push\("--copy"\)')
        self.assertRegex(self.main, r'o === "save" \|\| o === "copy-save"')
        self.assertRegex(self.main, r'args\.push\("--save"\)')
        # "edit" adds no flags: the builder never compares against "edit".
        self.assertNotIn('o === "edit"', self.main)

    def test_mode_output_validation_fallbacks(self):
        self.assertRegex(self.main, r'\["region", "window", "fullscreen"\]')
        self.assertRegex(self.main, r'\["edit", "copy", "save", "copy-save"\]')
        self.assertIn('? m : "region"', self.main)
        self.assertIn('? o : "edit"', self.main)

    def test_public_methods(self):
        for fn in ["capture", "captureDefault", "region", "windowCapture",
                   "fullscreen", "clipboard"]:
            self.assertRegex(self.main, rf"function\s+{fn}\s*\(")
        self.assertIn('root.launch(["nirisnap", "--clipboard"])', self.main)

    def test_ipc_surface(self):
        block = ipc_block(self.main)
        self.assertIn('target: "plugin:nirisnap"', block)
        methods = set(re.findall(r"function\s+(\w+)\s*\(", block))
        self.assertEqual(methods, {"toggle", "region", "window", "fullscreen",
                                   "clipboard"})

    def test_panel_closes_before_capture(self):
        self.assertIn("closePanel(pluginApi.panelOpenScreen)", self.main)
        self.assertRegex(self.main, r"interval:\s*200")
        self.assertRegex(self.main, r"repeat:\s*false")
        self.assertIn("if (panelCloseTimer.running)", self.main)

    def test_no_polling_or_daemon(self):
        self.assertNotIn("repeat: true", self.main)
        self.assertNotIn("Process {", self.main)
        self.assertNotIn("/bin/sh", self.main)
        self.assertNotIn("sh -c", self.main)

    def test_detached_argv_launches_only(self):
        # No shell interpolation: every execDetached call takes an array.
        for call in re.findall(r"Quickshell\.execDetached\(([^)]*)\)", self.main):
            self.assertTrue(call.strip().startswith("[") or call.strip().startswith("args"),
                            f"non-argv execDetached argument: {call}")


class TestBarWidget(unittest.TestCase):
    def test_button_contract(self):
        src = read("BarWidget.qml")
        self.assertIn("NIconButton", src)
        self.assertIn('icon: "camera"', src)
        self.assertIn("root.main?.captureDefault()", src)
        self.assertIn("pluginApi.openPanel(root.screen, root)", src)
        self.assertIn("BarService.openPluginSettings(root.screen, pluginApi.manifest)", src)
        self.assertIn("Style.getCapsuleHeightForScreen(screen?.name)", src)
        self.assertIn("BarService.getTooltipDirection(root.screen?.name)", src)
        self.assertIn("Color.resolveColorKey(root.iconColorKey)", src)


class TestPanel(unittest.TestCase):
    def test_panel_contract(self):
        src = read("Panel.qml")
        self.assertIn("readonly property bool allowAttach: true", src)
        self.assertIn("readonly property var geometryPlaceholder: panelContainer", src)
        self.assertIn("property real contentPreferredWidth: 380 * Style.uiScaleRatio", src)
        self.assertIn("NHeader", src)
        for mode in MODES:
            self.assertRegex(src, rf"main\?\.capture\(\"{mode}\"\)")
        self.assertIn("main?.clipboard()", src)
        self.assertIn("outlined: true", src)


class TestSettings(unittest.TestCase):
    def test_settings_contract(self):
        src = read("Settings.qml")
        self.assertEqual(src.count("NComboBox"), 2)
        self.assertIn("NColorChoice", src)
        self.assertIn("function saveSettings()", src)
        self.assertIn("pluginApi.saveSettings()", src)
        for key in ["defaultMode", "outputMode", "iconColor"]:
            self.assertIn(f"pluginApi.pluginSettings.{key} =", src)
        # No settings write outside saveSettings().
        save_start = src.index("function saveSettings()")
        self.assertNotIn("pluginApi.pluginSettings.", src[:save_start])


class TestStaleNames(unittest.TestCase):
    def test_no_upstream_compositor_or_branding(self):
        forbidden = ["oma" + "snap", "oma" + "rchy", "hypr" + "ctl", "hypr" + "land"]
        for name in ALL_FILES:
            text = read(name).lower()
            for word in forbidden:
                self.assertNotIn(word, text, f"stale name {word!r} in {name}")


if __name__ == "__main__":
    unittest.main()
