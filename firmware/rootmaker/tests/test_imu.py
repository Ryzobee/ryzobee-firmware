"""Production IMU public Interface over a deterministic register-level peer.

No device, physical I2C timing, PCB identity/orientation or self-test evidence.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ImuHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-imu-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "imu_test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-g", "-pthread",
            "-I", str(ROOT / "tests/imu_stubs"),
            "-I", str(ROOT / "components/ryz_board/include"),
            str(ROOT / "components/ryz_board/imu.c"),
            str(ROOT / "tests/imu_test.c"), "-lm", "-o", str(cls.binary),
        ], check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("IMU_PASS " + name, result.stdout)

    def test_config_fresh_settling_physical_mg_and_explicit_reinitialization(self):
        self.run_case("fresh")

    def test_exact_identity_unique_address_and_no_timeout_as_absence(self):
        self.run_case("identification")

    def test_bounded_reset_readback_failure_cleanup_and_retry(self):
        self.run_case("reset_config")

    def test_transfer_failure_invalidates_ready_and_never_returns_old_axes(self):
        self.run_case("read_failure")

    def test_concurrent_public_calls_fail_without_waiting(self):
        self.run_case("concurrent")

    def test_arguments_preinit_bus_init_failure_and_signed_extremes(self):
        self.run_case("bounds")

    def test_explicit_power_down_invalidates_samples_and_reports_failed_stop(self):
        self.run_case("deinit")


if __name__ == "__main__":
    unittest.main()
