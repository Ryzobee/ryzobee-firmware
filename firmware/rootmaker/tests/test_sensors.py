"""Real sensor worker, controlled OS/IMU Adapter; no board or sensor PASS."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_sensors"


class SensorsHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-sensors-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "sensors_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pthread", "-fsanitize=address,undefined", "-g"]
        for include in [ROOT / "tests/sensors_stubs", COMPONENT,
                        COMPONENT / "include", ROOT / "components/ryz_board/include"]:
            command.extend(["-I", str(include)])
        command.extend([str(COMPONENT / "ryz_sensors.c"),
                        str(ROOT / "tests/sensors_test.c"), "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SENSORS_PASS " + name, result.stdout)

    def test_start_failure_retry_and_idempotence(self):
        self.run_case("startup")

    def test_background_sampling_and_no_data_preserve_real_timestamp(self):
        self.run_case("sampling")

    def test_no_data_watchdog_invalidates_stale_xyz_before_reinitializing(self):
        self.run_case("watchdog")

    def test_concurrent_start_is_fail_closed_until_task_creation_finishes(self):
        self.run_case("concurrent_start")

    def test_cached_snapshot_remains_available_during_blocked_i2c(self):
        self.run_case("blocked_read")

    def test_initial_errors_and_no_first_sample_retry_without_fake_xyz(self):
        self.run_case("initial_failure")

    def test_transfer_and_info_errors_invalidate_before_retrying(self):
        self.run_case("transfer_failure")


if __name__ == "__main__":
    unittest.main()
