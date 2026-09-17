"""Actual time core + ESP callback adapter; only SDK/OS boundaries are replaced."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class TimeEspUtcTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.component = Path(__file__).resolve().parents[1]
        cls.temporary = tempfile.TemporaryDirectory(prefix="ryz-time-esp-utc-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "time-esp-utc"
        subprocess.run(
            [
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(cls.component / "test_host/esp_stubs"),
                "-I" + str(cls.component / "include"),
                "-I" + str(cls.component),
                str(cls.component / "test_host/time_esp_utc_test.c"),
                str(cls.component / "ryz_time.c"),
                str(cls.component / "ryz_time_esp.c"),
                "-o", str(cls.binary),
            ],
            check=True, timeout=60,
        )

    def run_case(self, case):
        completed = subprocess.run(
            [str(self.binary), case], text=True, capture_output=True, timeout=15,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn("TIME_ESP_UTC_PASS " + case, completed.stdout)

    def test_unstarted_and_uncalibrated_read_is_unknown_without_start(self):
        self.run_case("unknown")

    def test_registered_immediate_sync_preserves_subseconds_and_offline_utc(self):
        self.run_case("subseconds")

    def test_invalid_and_null_callbacks_revoke_until_fresh_sync(self):
        self.run_case("invalid")

    def test_clock_rollback_and_seconds_overflow_stay_unknown(self):
        self.run_case("clock")

    def test_busy_is_one_try_and_callback_captures_time_before_waiting(self):
        self.run_case("busy")


if __name__ == "__main__":
    unittest.main()
