"""Real system owner + provisioning core; controlled storage/radio Adapters."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / "components/ryz_system_services"
PROVISIONING = ROOT / "components/ryz_provisioning"


class SystemNetworkPreferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-system-network-preference-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "system_network_preference_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-pthread", "-fsanitize=address,undefined", "-g",
                   # Match the IDF constants without altering the shared fixture.
                   "-DESP_ERR_NOT_FOUND=0x105", "-DESP_ERR_NOT_SUPPORTED=0x106",
                   "-DESP_ERR_INVALID_RESPONSE=0x108"]
        for include in [ROOT / "tests/system_services_stubs", SERVICES, SERVICES / "include",
                        PROVISIONING, PROVISIONING / "include",
                        ROOT / "components/ryz_time/include", ROOT / "components/ryz_ota/include"]:
            command.extend(["-I", str(include)])
        command.extend([str(SERVICES / "ryz_system_services.c"),
                        str(SERVICES / "ryz_system_metrics.c"),
                        str(PROVISIONING / "ryz_provisioning.c"),
                        str(ROOT / "tests/system_network_preference_test.c"),
                        "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True,
                                timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SYSTEM_NETWORK_PREFERENCE_PASS " + name, result.stdout)

    def test_saved_off_boot_settles_without_starting_radio_or_online_dependents(self):
        self.run_case("saved_off")

    def test_missing_ssid_or_scan_failure_settles_ap_without_connection_window_or_retry(self):
        for case in ["scan_missing", "scan_failed"]:
            with self.subTest(case=case):
                self.run_case(case)

    def test_on_save_failure_is_failed_operation_preserves_off_and_needs_explicit_retry(self):
        self.run_case("on_save_failure")

    def test_off_save_failure_is_failed_operation_preserves_online_and_needs_explicit_retry(self):
        self.run_case("off_save_failure")

    def test_invalid_saved_preference_rolls_back_and_retries_init_without_starting_radio(self):
        for case in ["load_failure", "load_failure_on"]:
            with self.subTest(case=case):
                self.run_case(case)

    def test_ota_wait_prevents_preference_write_until_cleanup_is_confirmed(self):
        self.run_case("ota_wait")


if __name__ == "__main__":
    unittest.main()
