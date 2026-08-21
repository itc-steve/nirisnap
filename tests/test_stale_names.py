import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = ("oma" + "snap", "oma" + "rchy", "hypr" + "ctl", "hypr" + "land")


class StaleNameTest(unittest.TestCase):
    def test_runtime_build_and_tests_have_no_upstream_coupling(self):
        paths = [
            ROOT / "src",
            ROOT / "plugin",
            ROOT / "packaging",
            ROOT / "CMakeLists.txt",
            ROOT / "Makefile",
            ROOT / "nirisnap.desktop",
            ROOT / ".github/workflows/build-linux.yml",
            ROOT / "tests",
        ]
        findings = []
        for root in paths:
            files = root.rglob("*") if root.is_dir() else [root]
            for path in files:
                if not path.is_file() or path == Path(__file__) or "__pycache__" in path.parts:
                    continue
                try:
                    text = path.read_text(encoding="utf-8").lower()
                except UnicodeDecodeError:
                    continue
                for term in FORBIDDEN:
                    if term in text:
                        findings.append(f"{path.relative_to(ROOT)}: {term}")
        self.assertEqual(findings, [], "stale coupling:\n" + "\n".join(findings))


if __name__ == "__main__":
    unittest.main()
