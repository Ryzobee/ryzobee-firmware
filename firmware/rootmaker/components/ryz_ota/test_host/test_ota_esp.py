"""Real OTA core + ESP Adapter with deterministic SDK/OS replacements.

The SDK replacement consumes valid handles on abort/finish even on errors,
matching the audited IDF 5.5.4 branches. No network or real allocator is run.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest


class OtaEspTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="ryzobee-ota-esp-")
        component = Path(__file__).resolve().parents[1]
        cls.binary = Path(cls.temporary.name) / "ota-esp-test"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-pthread",
            "-I" + str(component / "test_host/esp_stubs"),
            "-I" + str(component / "test_host/stubs"),
            "-I" + str(component / "include"), "-I" + str(component),
            str(component / "test_host/ota_esp_test.c"),
            str(component / "ryz_ota.c"), str(component / "ryz_ota_esp.c"),
            "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RYZ_OTA_ESP_PASS", result.stdout)

    def test_candidate_abort_window(self):
        self.run_case("candidate-abort-window")

    def test_running_info(self):
        self.run_case("running-info")

    def test_all_running_states_and_factory(self):
        for case in ("meta-new", "meta-pending", "meta-valid", "meta-invalid",
                     "meta-aborted", "meta-undefined", "meta-factory", "meta-label16",
                     "meta-no-state", "meta-state-unsupported"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_metadata_initialization_errors_release_and_retry(self):
        for case in ("meta-state-error", "meta-state-malformed", "meta-running-missing",
                     "meta-running-malformed"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_actual_ab_partition_layout(self):
        for case in ("meta-ab-missing-a", "meta-ab-missing-b", "meta-ab-missing-data",
                     "meta-ab-type", "meta-ab-subtype", "meta-ab-address", "meta-ab-size",
                     "meta-ab-data-size", "meta-ab-overflow", "meta-ab-overlap",
                     "meta-ab-chip", "meta-ab-erase", "meta-ab-label"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_confirmation_observes_same_running_image_valid(self):
        for case in ("confirm-real-success", "confirm-real-write-error",
                     "confirm-real-readback-error", "confirm-real-readback-missing",
                     "confirm-real-readback-pending", "confirm-real-changed-after",
                     "confirm-real-already-valid", "confirm-real-before-missing",
                     "confirm-real-before-unsupported", "confirm-real-before-invalid",
                     "confirm-real-changed-before"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_error_consumption_and_retry(self):
        for case in ("abort-error", "begin-error", "begin-null-success", "begin-unknown",
                     "desc-error", "perform-error", "incomplete", "finish-error"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_network_hold_boundaries(self):
        for case in ("hold-init", "hold-queued", "hold-begin", "hold-desc", "hold-perform",
                     "hold-finish", "hold-verify-lock"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_allocation_failure_and_normal_completion(self):
        for case in ("init-mutex-failure", "init-description-failure", "task-failure", "success"):
            with self.subTest(case=case):
                self.run_case(case)


if __name__ == "__main__":
    unittest.main()
