"""Real time core + system owner; controlled pthread/SNTP and OTA observations."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / "components/ryz_system_services"
TIME = ROOT / "components/ryz_time"


class SystemTimeIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-system-time-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "system_time_integration_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-pthread", "-fsanitize=address,undefined", "-g",
                   # Same IDF error value as ryz_time/test_host/stubs/esp_err.h;
                   # leave the reused system-services header unchanged.
                   "-DESP_ERR_INVALID_RESPONSE=0x108"]
        for include in [ROOT / "tests/system_services_stubs", SERVICES, SERVICES / "include",
                        TIME, TIME / "include", ROOT / "components/ryz_provisioning/include",
                        ROOT / "components/ryz_ota/include"]:
            command.extend(["-I", str(include)])
        command.extend([str(SERVICES / "ryz_system_services.c"),
                        str(SERVICES / "ryz_system_metrics.c"), str(TIME / "ryz_time.c"),
                        str(ROOT / "tests/system_time_integration_test.c"),
                        "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True,
                                timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SYSTEM_TIME_INTEGRATION_PASS " + name, result.stdout)

    def test_async_timeout_returns_error_once_then_owner_waits_five_seconds(self):
        self.run_case("timeout_retry")

    def test_saved_off_settles_boot_without_starting_or_waiting_for_ntp(self):
        self.run_case("off_boot")

    def test_offline_bypasses_backoff_and_reconnect_receives_a_new_sync_window(self):
        self.run_case("offline_backoff")

    def test_late_valid_clock_callback_updates_ota_prerequisites_without_network_revision(self):
        self.run_case("late_sync")

    def test_invalid_async_clock_revokes_ota_clock_and_enters_owner_backoff_once(self):
        self.run_case("invalid_sync_backoff")


if __name__ == "__main__":
    unittest.main()
