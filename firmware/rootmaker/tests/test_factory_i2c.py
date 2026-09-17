"""Actual factory script through the public Lua app entrypoint and real LVGL."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-host/simulator"


class FactoryI2CTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["cmake", "-S", "host/pixels", "-B", str(BUILD),
                        "-DRYZ_IDF_BUILD=" + str(ROOT / "build")], cwd=ROOT,
                       check=True, capture_output=True, timeout=60)
        subprocess.run(["cmake", "--build", str(BUILD), "--target", "i2c_script_pixels", "-j4"],
                       cwd=ROOT, check=True, capture_output=True, timeout=120)

    def test_editable_factory_copy_is_same_source_within_quota(self):
        source = (ROOT / "scripts/tool_i2c.lua").read_bytes()
        self.assertEqual(source, (ROOT / "fs/tool_i2c.lua").read_bytes())
        self.assertTrue(source.startswith(b"-- ryz-app/1\n"))
        self.assertLessEqual(len(source), 16384)
        self.assertNotIn(b"placeholder", source)

    def test_board_scan_reports_ack_without_inventing_identity(self):
        result = subprocess.run([str(BUILD / "i2c_script_pixels"), str(ROOT / "fs/tool_i2c.lua"),
                                 "happy", str(BUILD)], text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("I2C_SCRIPT_PASS happy", result.stdout)

    def test_pin_drafts_require_explicit_confirmation_and_restore_shared_bus(self):
        result = subprocess.run([str(BUILD / "i2c_script_pixels"), str(ROOT / "fs/tool_i2c.lua"),
                                 "pins", str(BUILD)], text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("I2C_SCRIPT_PASS pins", result.stdout)

    def test_failures_cancellation_empty_and_full_address_history(self):
        for case in ("errors", "close-errors", "empty", "cancel", "scroll", "picker", "repeat"):
            with self.subTest(case=case):
                result = subprocess.run([str(BUILD / "i2c_script_pixels"),
                                         str(ROOT / "fs/tool_i2c.lua"), case, str(BUILD)],
                                        text=True, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("I2C_SCRIPT_PASS " + case, result.stdout)


if __name__ == "__main__":
    unittest.main()
