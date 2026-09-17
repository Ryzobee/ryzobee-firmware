"""RGB public Interface and real worker; no electrical/optical PASS claim."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_rgb"


class RgbHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-rgb-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "rgb_test"
        cls.platform_binary = Path(cls.directory.name) / "rgb_platform_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pthread", "-fsanitize=address,undefined", "-g"]
        for include in [ROOT / "tests/rgb_stubs", COMPONENT, COMPONENT / "include"]:
            command.extend(["-I", str(include)])
        platform_command = command + [str(COMPONENT / "ryz_rgb.c"),
                                      str(COMPONENT / "ryz_rgb_esp.c"),
                                      str(ROOT / "tests/rgb_stubs/platform_test.c"),
                                      "-o", str(cls.platform_binary)]
        command.extend([str(COMPONENT / "ryz_rgb.c"),
                        str(ROOT / "tests/rgb_test.c"), "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=30)
        subprocess.run(platform_command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RGB_PASS " + name, result.stdout)

    def test_lazy_creation_retry_and_queued_work_cannot_be_overwritten(self):
        self.run_case("startup")

    def test_background_raw_rgb_completion_and_black_are_actual_driver_writes(self):
        self.run_case("success_off")

    def test_sending_revokes_known_and_failure_keeps_history_until_explicit_retry(self):
        self.run_case("sending_recovery")

    def test_first_failure_never_assumes_black_and_does_not_retry_automatically(self):
        self.run_case("initial_failure")

    def test_concurrent_startup_cannot_steal_the_first_request(self):
        self.run_case("concurrent_start")

    def test_competing_submitters_admit_exactly_one_color(self):
        self.run_case("submit_race")

    def test_binary_wakes_do_not_duplicate_or_drop_serial_generations(self):
        self.run_case("generations")

    def test_cleanup_supersedes_queued_without_sending_a_color_or_black_frame(self):
        self.run_case("cleanup_queued")

    def test_cleanup_keeps_color_history_and_old_token_cannot_stop_new_request(self):
        self.run_case("cleanup_history")

    def test_cleanup_waits_for_inflight_write_before_teardown_and_keeps_actual_completion(self):
        self.run_case("cleanup_inflight")

    def test_cleanup_after_inflight_write_failure_preserves_original_error(self):
        self.run_case("cleanup_inflight_failed")

    def test_cleanup_failure_retains_resources_and_requires_same_token_explicit_retry(self):
        self.run_case("cleanup_failure_retry")

    def test_cleanup_success_with_retained_resources_is_not_reported_as_cleaned(self):
        self.run_case("cleanup_inconsistent")

    def test_cleanup_and_next_submit_cannot_both_win_the_old_generation(self):
        self.run_case("cleanup_submit_race")

    def test_real_esp_adapter_cleans_partial_resources_and_retries_task_creation(self):
        result = subprocess.run([str(self.platform_binary)], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RGB_PASS platform_alloc", result.stdout)


if __name__ == "__main__":
    unittest.main()
