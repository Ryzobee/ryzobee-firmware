"""Production BLE model/admission projection with a controlled service peer."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WorkbenchBleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-wb-ble-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "ble-test"
        includes = [ROOT / "tests/imu_stubs", ROOT / "tests/workbench_scripts_stubs"]
        for name in ("ryz_workbench", "ryz_system_ui", "ryz_ble"):
            includes += [ROOT / "components" / name, ROOT / "components" / name / "include"]
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                   "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined"]
        command += [part for path in includes for part in ("-I", str(path))]
        command += [str(ROOT / "tests/workbench_ble_test.c"),
                    str(ROOT / "components/ryz_workbench/workbench_ble.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True,
                                timeout=5, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_BLE_PASS " + name, result.stdout)

    def test_every_phase_projects_real_state_without_gatt_or_navigation(self):
        self.run_case("model")

    def test_unavailable_errors_clear_stale_peer_code_and_operation(self):
        self.run_case("invalid")

    def test_unavailable_preserves_only_actual_failure_code_without_radio_claims(self):
        self.run_case("unavailable-error")

    def test_only_confirmed_actions_forward_the_exact_displayed_operation(self):
        self.run_case("commands")

    def test_numeric_confirmation_binds_operation_and_six_digit_value(self):
        self.run_case("numeric")

    def test_boot_waits_for_milestone_but_task_creation_error_releases_ui(self):
        self.run_case("boot")


if __name__ == "__main__":
    unittest.main()
