import shlex
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MARKER = "QT_QPA_PLATFORM=offscreen"


def smoke_command(build_dir):
    args = ["make", "-n", "smoke", f"BUILD_DIR={build_dir}"]
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    joined = result.stdout.replace("\\\n", " ")
    lines = [" ".join(line.split()) for line in joined.splitlines() if MARKER in line]
    assert len(lines) == 1, joined
    return shlex.split(lines[0])


class MakefileSmokePathTest(unittest.TestCase):
    def test_absolute_build_dir_runs_quoted_absolute_binary(self):
        command = smoke_command("/tmp/example-absolute-build-dir")
        self.assertEqual(
            command,
            [MARKER, "/tmp/example-absolute-build-dir/nirisnap-smoke",
             "/tmp/example-absolute-build-dir/nirisnap-smoke-output"],
        )

    def test_relative_build_dir_has_no_dot_slash_prefix(self):
        command = smoke_command("build")
        self.assertEqual(
            command,
            [MARKER, "build/nirisnap-smoke", "build/nirisnap-smoke-output"],
        )


if __name__ == "__main__":
    unittest.main()
