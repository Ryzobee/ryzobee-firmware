"""Host contract test for the public Ryzobee time seam."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class TimeStateTest(unittest.TestCase):
    def test_network_gated_sntp_state_machine(self):
        self.run_case("time_state_test.c", "RYZ_TIME_STATE_PASS")

    def test_missing_ntp_response_has_a_bounded_sync_window(self):
        self.run_case("time_timeout_test.c", "RYZ_TIME_TIMEOUT_PASS")

    def test_late_sync_offline_fresh_windows_and_invalid_clock_report(self):
        self.run_case("time_late_sync_test.c", "RYZ_TIME_LATE_SYNC_PASS")

    def test_start_return_cannot_overwrite_newer_clock_callbacks(self):
        self.run_case("time_start_callback_test.c", "RYZ_TIME_START_CALLBACK_PASS")

    def test_nonwaiting_current_utc_precision_faults_and_concurrent_sync(self):
        self.run_case("time_utc_test.c", "RYZ_TIME_UTC_PASS")

    def run_case(self, source, marker):
        component = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="ryzobee-time-test-") as temporary:
            binary = Path(temporary) / "time-state-test"
            command = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-pthread",
                "-I" + str(component / "test_host/stubs"),
                "-I" + str(component / "include"),
                "-I" + str(component),
                str(component / "test_host" / source),
                str(component / "test_host/fake_platform.c"),
                str(component / "ryz_time.c"),
                "-o",
                str(binary),
            ]
            subprocess.run(command, check=True, timeout=60)
            completed = subprocess.run(
                [str(binary)], text=True, capture_output=True, check=False, timeout=15
            )
            self.assertEqual(
                completed.returncode, 0, completed.stderr + completed.stdout
            )
            self.assertIn(marker, completed.stdout)


if __name__ == "__main__":
    unittest.main()
