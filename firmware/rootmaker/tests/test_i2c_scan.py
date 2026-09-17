"""Real scan worker with controlled OS/probe Adapters, not board evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_i2c_scan"


class I2cScanHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-i2c-scan-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "i2c_scan_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pthread", "-fsanitize=address,undefined", "-g"]
        for include in [ROOT / "tests/i2c_scan_stubs", COMPONENT,
                        COMPONENT / "include", ROOT / "components/ryz_tool_pins/include"]:
            command.extend(["-I", str(include)])
        base_command = command.copy()
        command.extend([str(COMPONENT / "ryz_i2c_scan.c"),
                        str(ROOT / "tests/i2c_scan_test.c"), "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=30)
        cls.platform_binary = Path(cls.directory.name) / "i2c_scan_platform_test"
        base_command.extend([str(COMPONENT / "ryz_i2c_scan.c"),
                             str(COMPONENT / "ryz_i2c_scan_esp.c"),
                             str(ROOT / "tests/i2c_scan_stubs/platform_test.c"),
                             "-o", str(cls.platform_binary)])
        subprocess.run(base_command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("I2C_SCAN_PASS " + name, result.stdout)

    def test_lazy_start_retry_and_active_rejection(self):
        self.run_case("startup")

    def test_complete_traversal_reports_all_112_addresses_not_fake_empty(self):
        self.run_case("complete")

    def test_busy_is_bounded_to_448_attempts_and_never_counts_as_nack(self):
        self.run_case("busy")

    def test_only_contention_retries_and_real_errors_remain_per_address(self):
        self.run_case("mixed")

    def test_queued_cancellation_requires_worker_ack_without_any_probe(self):
        self.run_case("cancel_queued")

    def test_inflight_cancel_discards_result_and_old_id_cannot_touch_restart(self):
        self.run_case("cancel_inflight")

    def test_cancel_between_addresses_starts_no_later_probe(self):
        self.run_case("cancel_gap")

    def test_cancel_during_contention_delay_does_not_finish_current_address(self):
        self.run_case("cancel_retry")

    def test_board_init_failure_is_failed_not_empty_and_next_request_retries(self):
        self.run_case("init_failure")

    def test_concurrent_startup_rejects_until_first_request_is_queued(self):
        self.run_case("concurrent_start")

    def test_configuration_cas_is_software_only_and_failures_keep_prior_config(self):
        self.run_case("configure")

    def test_cancel_retries_failed_resource_cleanup_without_starting_another_scan(self):
        self.run_case("cleanup_retry")

    def test_idle_configuration_changes_do_not_relabel_historical_scan_results(self):
        self.run_case("configure_history")

    def test_releasing_blocks_start_and_configure_and_late_cancel_waits_for_cleanup(self):
        self.run_case("release_gate")

    def test_queued_cancel_still_waits_for_cleanup_without_admitting_a_bus(self):
        self.run_case("queued_release_gate")

    def test_explicit_restart_after_teardown_failure_preserves_held_until_worker_recovers(self):
        self.run_case("held_restart")

    def test_configure_retries_failed_worker_creation_without_committing_a_revision(self):
        self.run_case("configure_startup")

    def test_first_configure_owns_creation_and_competing_cas_commits_only_one_revision(self):
        self.run_case("configure_concurrent")

    def test_real_esp_adapter_cleans_partial_allocations_and_task_failure(self):
        result = subprocess.run([str(self.platform_binary)], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("I2C_SCAN_PASS platform_alloc", result.stdout)


if __name__ == "__main__":
    unittest.main()
