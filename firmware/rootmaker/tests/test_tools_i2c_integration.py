"""Real broker -> scan core with controlled bus/OS, not physical I2C evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ToolsI2cIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-tools-i2c-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "tools_i2c"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-pthread", "-fsanitize=address,undefined", "-g",
                   "-I" + str(ROOT / "tests/i2c_scan_stubs"),
                   "-I" + str(ROOT / "components/ryz_i2c_scan")]
        for component in ("ryz_tools", "ryz_i2c_scan", "ryz_tool_pins", "ryz_monitor", "ryz_rgb"):
            command += ["-I" + str(ROOT / "components" / component / "include")]
        scanner_object = Path(cls.directory.name) / "scan.o"
        # Rename only this public symbol in the real scanner object, allowing
        # a deterministic scheduling gate without GNU --wrap on macOS. The
        # gate always delegates to the real function and never alters results.
        subprocess.run(command + ["-Dryz_i2c_scan_cancel=ryz_i2c_scan_cancel_real", "-c",
                       str(ROOT / "components/ryz_i2c_scan/ryz_i2c_scan.c"),
                       "-o", str(scanner_object)], check=True, timeout=30)
        command += [str(ROOT / "components/ryz_tools/ryz_tools.c"),
                    str(scanner_object),
                    str(ROOT / "tests/tools_i2c_integration_test.c"),
                    "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], text=True, capture_output=True,
                                timeout=15, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TOOLS_I2C_PASS " + name, result.stdout)

    def test_claim_config_start_and_exact_cancel_wait_for_real_worker_release(self):
        self.case("claim_scan_release")

    def test_failed_held_cleanup_retains_owner_until_explicit_retry_and_old_close_cannot_stop_next_scan(self):
        self.case("cleanup_failed_retry")

    def test_config_only_claim_does_not_relabel_cancel_or_adopt_historical_scan(self):
        self.case("config_only_history")

    def test_closing_queued_scan_performs_cleanup_without_begin_or_probe(self):
        self.case("queued_close")

    def test_active_external_scan_cannot_be_adopted_or_cancelled_by_application(self):
        self.case("external_active")

    def test_natural_completion_between_snapshot_and_cancel_releases_same_identity(self):
        self.case("completion_race")


if __name__ == "__main__":
    unittest.main()
