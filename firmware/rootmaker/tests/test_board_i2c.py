"""Actual shared bus, touch init/read and protocol against a pthread/IDF boundary.

Host verifies passed budgets and serialized driver calls, not electrical timing.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "components" / "ryz_board"
STUBS = ROOT / "tests" / "board_i2c_stubs"


class BoardI2CHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-board-i2c-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binaries = {}
        for tick_ms in [1, 50]:
            binary = Path(cls.directory.name) / ("board-i2c-" + str(tick_ms))
            command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                       "-Werror", "-pthread", "-g", "-fsanitize=address,undefined",
                       "-DHOST_TICK_MS=" + str(tick_ms), "-I" + str(STUBS),
                       "-I" + str(BOARD / "include"),
                       str(ROOT / "tests" / "board_i2c_test.c"),
                       str(BOARD / "board_i2c.c"), str(BOARD / "touch.c"),
                       str(BOARD / "touch_protocol.c"), "-o", str(binary)]
            subprocess.run(command, check=True, timeout=60)
            cls.binaries[tick_ms] = binary

    def run_case(self, name, tick_ms=1):
        result = subprocess.run(
            [str(self.binaries[tick_ms]), name], capture_output=True, text=True,
            timeout=15, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("BOARD_I2C_PASS " + name, result.stdout)

    def test_init_failures_retry_concurrent_start_and_single_bus(self):
        self.run_case("init")

    def test_lazy_endpoints_args_registration_failure_and_transfer_errors(self):
        self.run_case("transfers")

    def test_nonzero_mutex_budget_when_twenty_ms_rounds_to_zero_ticks(self):
        self.run_case("transfers", tick_ms=50)

    def test_probe_range_no_registration_three_ms_and_timeout_not_absence(self):
        self.run_case("probe")

    def test_real_threads_serialize_devices_and_bound_contention(self):
        self.run_case("serialization")

    def test_try_probe_distinguishes_mutex_busy_from_physical_timeout_and_nack(self):
        self.run_case("try_probe")

    def test_cst816d_config_reset_awake_and_error_clears_old_touch(self):
        self.run_case("touch_d")

    def test_cst816t_identity_remains_supported(self):
        self.run_case("touch_t")

    def test_every_touch_init_failure_keeps_shared_bus_and_imu_then_retries(self):
        for stage in range(12):
            with self.subTest(stage=stage):
                self.run_case("touch_failure_" + str(stage))


if __name__ == "__main__":
    unittest.main()
