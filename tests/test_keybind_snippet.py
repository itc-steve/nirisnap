import re
import unittest
from pathlib import Path


SNIPPET = Path(__file__).resolve().parents[1] / "packaging/nirisnap-keybinds.kdl"


class KeybindSnippetTest(unittest.TestCase):
    def test_ctrl_shift_1_launches_region_capture(self):
        text = SNIPPET.read_text(encoding="utf-8")
        self.assertEqual(text.count("binds {"), 1)
        self.assertEqual(text.count("CTRL+Shift+1 "), 1)
        self.assertEqual(text.count("{"), text.count("}"))
        bind = next(
            line for line in text.splitlines() if re.match(r"\s*CTRL\+Shift\+1\s", line)
        )
        self.assertIn("repeat=false", bind)
        self.assertEqual(
            re.findall(r'"([^"]*)"', bind),
            ["Screenshot: Nirisnap", "nirisnap", "--capture-region"],
        )


if __name__ == "__main__":
    unittest.main()
