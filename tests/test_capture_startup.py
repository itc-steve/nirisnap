"""Offscreen startup regression: instrument real main, use only synthetic commands.

Build first; NIRISNAP_TEST_BUILD_DIR selects a separate CMake/Ninja build.
The harness only observes Qt Show events; production startup stays unchanged.
"""

import json
import os
import shlex
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("NIRISNAP_TEST_BUILD_DIR", ROOT / "build")).resolve()

HARNESS = r'''
#include "editor.hpp"
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QTimer>
#include <QWindow>

class StartupApplication : public QApplication {
public:
  StartupApplication(int &argc, char **argv) : QApplication(argc, argv) {
    installEventFilter(this);
    QTimer::singleShot(0, this, [] {
      QFile file(qEnvironmentVariable("STARTUP_LOOP"));
      file.open(QIODevice::WriteOnly);
    });
    connect(this, &QCoreApplication::aboutToQuit, this, [] {
      QFile file(qEnvironmentVariable("STARTUP_QUIT"));
      file.open(QIODevice::WriteOnly);
    });
    QTimer::singleShot(15000, this, [] { QCoreApplication::exit(99); });
  }
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::Show &&
        (qobject_cast<CaptureEditor *>(object) || qobject_cast<QWindow *>(object))) {
      bool ready = false;
      for (QWidget *widget : topLevelWidgets())
        if (auto *editor = qobject_cast<CaptureEditor *>(widget))
          ready = !editor->captureData().source.isNull();
      QFile file(qEnvironmentVariable("STARTUP_SHOW"));
      if (file.open(QIODevice::WriteOnly | QIODevice::Append))
        file.write(ready ? "ready\n" : "empty\n");
    }
    return QApplication::eventFilter(object, event);
  }
};
#define QApplication StartupApplication
#define main captureMain
#include "main.cpp"
#undef main
#undef QApplication
int main(int argc, char **argv) { return captureMain(argc, argv); }
'''


@unittest.skipUnless(
    (BUILD / "compile_commands.json").is_file() and (BUILD / "build.ninja").is_file(),
    "CMake/Ninja build required",
)
class CaptureStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Keep editor.hpp layout and linked objects in sync after source edits.
        subprocess.run(
            ["cmake", "--build", str(BUILD), "--target", "nirisnap", "--parallel", "2"],
            check=True, capture_output=True,
        )
        cls.harness_dir = tempfile.TemporaryDirectory(prefix="startup-", dir=BUILD)
        cls.addClassCleanup(cls.harness_dir.cleanup)
        directory = Path(cls.harness_dir.name)
        source = directory / "startup.cpp"
        source.write_text(HARNESS)
        commands = json.loads((BUILD / "compile_commands.json").read_text())
        entry = next(item for item in commands if item["file"] == str(ROOT / "src/main.cpp"))
        compile_args = shlex.split(entry["command"])
        main_object = compile_args[compile_args.index("-o") + 1]
        compile_args[compile_args.index("-o") + 1] = str(directory / "startup.o")
        compile_args[compile_args.index("-c") + 1] = str(source)
        subprocess.run(compile_args, cwd=BUILD, check=True, capture_output=True)
        # Reuse CMake's actual link flags and objects, replacing only main.
        link_line = subprocess.check_output(
            ["ninja", "-t", "commands", "nirisnap"], cwd=BUILD, text=True
        ).splitlines()[-1]
        link_args = shlex.split(link_line.split("&&")[1])
        link_args[link_args.index(main_object)] = str(directory / "startup.o")
        cls.binary = directory / "nirisnap"
        link_args[link_args.index("-o") + 1] = str(cls.binary)
        subprocess.run(link_args, cwd=BUILD, check=True, capture_output=True)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="synthetic-", dir=BUILD)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        self.env = os.environ | {
            "QT_QPA_PLATFORM": "offscreen",
            "PATH": str(self.root) + os.pathsep + os.environ["PATH"],
            "XDG_RUNTIME_DIR": str(runtime),
            "HOME": str(self.root),
            "XDG_CONFIG_HOME": str(self.root / "config"),
            "XDG_DATA_HOME": str(self.root / "data"),
            "XDG_STATE_HOME": str(self.root / "state"),
            "XDG_CACHE_HOME": str(self.root / "cache"),
            "NIRISNAP_SCREENSHOT_DIR": str(self.root / "screenshots"),
            "STARTUP_ROOT": str(self.root),
            "STARTUP_SHOW": str(self.root / "show"),
            "STARTUP_LOOP": str(self.root / "loop"),
            "STARTUP_QUIT": str(self.root / "quit"),
        }
        self.command("niri", '''#!/usr/bin/env python3
import os, sys
from pathlib import Path
root = Path(os.environ["STARTUP_ROOT"])
with (root / "niri.log").open("a") as log: log.write(" ".join(sys.argv[1:]) + "\\n")
responses = {
    "msg -j focused-output": '{"name":"SYNTHETIC","modes":[{"width":3,"height":2}],"current_mode":0,"logical":{"x":0,"y":0,"width":3,"height":2,"scale":1}}',
    "msg -j workspaces": '[]',
    "msg -j windows": '[]',
}
print(responses[" ".join(sys.argv[1:])])
''')
        self.command("grim", '''#!/usr/bin/env python3
import os, sys, time
from pathlib import Path
root = Path(os.environ["STARTUP_ROOT"])
(root / "started").touch()
with (root / "grim.log").open("a") as log: log.write("capture\\n")
while not (root / "release").exists(): time.sleep(0.01)
if os.environ.get("STARTUP_FAIL"): sys.exit(3)
(root / "done").touch()
sys.stdout.buffer.write(b"P6\\n3 2\\n255\\n" + b"\\x12\\x34\\x56" * 6)
''')
        self.command("notify-send", "#!/bin/sh\nexit 0\n")
        for name in ("wl-copy", "wl-paste", "xdg-open"):
            self.command(name, "#!/bin/sh\nexit 64\n")
        self.child = None
        self.addCleanup(self.stop)

    def command(self, name, content):
        path = self.root / name
        path.write_text(content)
        path.chmod(0o700)

    def start(self, *args):
        self.child = subprocess.Popen(
            [str(self.binary), *args], env=self.env,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )

    def wait_for(self, name):
        deadline = time.monotonic() + 5
        while not (self.root / name).exists():
            if self.child.poll() is not None:
                self.fail(f"Child exited {self.child.returncode}: {self.child.stderr.read()}")
            if time.monotonic() > deadline:
                self.fail(f"Timed out waiting for {name}")
            time.sleep(0.01)

    def stop(self):
        if self.child:
            (self.root / "release").touch()
            if self.child.poll() is None:
                self.child.terminate()
            try:
                self.child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait()
            self.child.stderr.close()

    def assert_hidden_during_capture(self):
        self.wait_for("started")
        self.wait_for("loop")  # GUI event loop runs while worker waits on grim.
        self.assertFalse((self.root / "show").exists(), "Overlay mapped before capture completed")

    def test_overlay_shown_only_after_successful_capture(self):
        self.start("--capture-region")
        self.assert_hidden_during_capture()
        (self.root / "release").touch()
        self.wait_for("show")
        self.assertTrue((self.root / "done").exists())
        self.assertNotIn("empty", (self.root / "show").read_text())

    def test_capture_failure_never_shows_overlay(self):
        self.env["STARTUP_FAIL"] = "1"
        self.start("--capture-region")
        self.assert_hidden_during_capture()
        (self.root / "release").touch()
        self.assertEqual(self.child.wait(timeout=5), 1)
        self.assertFalse((self.root / "show").exists())

    def test_fullscreen_quick_output_never_shows_overlay(self):
        self.start("--capture-fullscreen", "--save")
        self.assert_hidden_during_capture()
        (self.root / "release").touch()
        self.assertEqual(self.child.wait(timeout=5), 0, self.child.stderr.read())
        self.assertFalse((self.root / "show").exists())
        self.assertTrue(list(self.root.rglob("screenshot-*.png")))
        self.assertNotIn("windows", (self.root / "niri.log").read_text())
        self.assertNotIn("workspaces", (self.root / "niri.log").read_text())

    def test_file_editor_shows_without_capture(self):
        image = self.root / "input.ppm"
        image.write_bytes(b"P6\n3 2\n255\n" + b"\x12\x34\x56" * 6)
        self.start("--file", str(image))
        self.wait_for("show")
        self.assertNotIn("empty", (self.root / "show").read_text())
        self.assertFalse((self.root / "started").exists())
        self.assertFalse((self.root / "niri.log").exists())

    def test_second_capture_dismisses_hidden_instance_without_capturing(self):
        self.start("--capture-region")
        self.assert_hidden_during_capture()
        second = subprocess.run(
            [str(self.binary), "--capture-fullscreen", "--save"],
            env=self.env, capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(second.returncode, 0, second.stderr)
        self.wait_for("quit")
        (self.root / "release").touch()
        self.assertEqual(self.child.wait(timeout=5), 0)
        self.assertEqual((self.root / "grim.log").read_text(), "capture\n")
        self.assertFalse((self.root / "show").exists())
        self.assertFalse(list(self.root.rglob("screenshot-*.png")))

    def test_signal_during_capture_never_shows_overlay(self):
        self.start("--capture-region")
        self.assert_hidden_during_capture()
        self.child.terminate()
        self.wait_for("quit")
        (self.root / "release").touch()
        self.assertEqual(self.child.wait(timeout=5), 0)
        self.assertFalse((self.root / "show").exists())


if __name__ == "__main__":
    unittest.main()
