"""Factory Lua tools omit exit instructions while keeping synchronized copies."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FactoryExitHintsTest(unittest.TestCase):
    def test_no_timed_exit_hint_in_factory_tools(self):
        for name in ("tool_monitor.lua", "tool_i2c.lua", "tool_hardware.lua"):
            with self.subTest(script=name):
                source = (ROOT / "scripts" / name).read_text()
                self.assertEqual(source, (ROOT / "fs" / name).read_text())
                self.assertNotRegex(source.upper(), r"HOLD\s+BOOT\s+\d+S")
                self.assertNotRegex(source.upper(), r"HOLD[^\n'\"]*EXIT")


if __name__ == "__main__":
    unittest.main()
