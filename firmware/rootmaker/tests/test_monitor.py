"""Public Monitor core + real fixed stream; OS/UART/logger are external Adapters."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_monitor"


class MonitorHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-monitor-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "monitor_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-pthread", "-fsanitize=address,undefined", "-g"]
        for include in [ROOT / "tests/monitor_stubs", COMPONENT, COMPONENT / "include"]:
            command += ["-I", str(include)]
        cls.platform_binary = Path(cls.directory.name) / "monitor_platform_test"
        platform_command = command + [str(COMPONENT / "ryz_monitor.c"),
            str(COMPONENT / "ryz_monitor_stream.c"), str(COMPONENT / "ryz_monitor_esp.c"),
            str(ROOT / "tests/monitor_stubs/platform_test.c"), "-o", str(cls.platform_binary)]
        command += [str(COMPONENT / "ryz_monitor.c"), str(COMPONENT / "ryz_monitor_stream.c"),
                    str(ROOT / "tests/monitor_test.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run(platform_command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MONITOR_PASS " + name, result.stdout)

    def test_start_is_lazy_async_and_cannot_be_overwritten(self):
        self.run_case("startup")

    def test_pause_clear_stop_and_restart_use_bounded_views_and_never_mix_sessions(self):
        self.run_case("system_lifecycle")

    def test_uart_binary_pages_events_fairness_and_atomic_running_source_change(self):
        self.run_case("uart_capture")

    def test_failed_candidate_restores_previous_capture_without_committing_config(self):
        self.run_case("restore")

    def test_failed_restore_is_failed_not_fake_running(self):
        self.run_case("restore_fail")

    def test_failed_cleanup_keeps_resources_and_same_session_stop_retries_only_release(self):
        self.run_case("cleanup_retry")

    def test_stop_supersedes_queued_start_without_installing_a_source(self):
        self.run_case("queued_stop")

    def test_stop_during_native_begin_prevents_late_capture(self):
        self.run_case("cancel_begin")

    def test_stop_during_candidate_begin_keeps_previous_committed_config(self):
        self.run_case("cancel_candidate")

    def test_stop_during_restore_prevents_reopening_capture(self):
        self.run_case("cancel_restore")

    def test_clear_expires_pre_poll_token_and_drops_inflight_old_bytes(self):
        self.run_case("clear_inflight")

    def test_stop_discards_inflight_sample_before_resource_release(self):
        self.run_case("stop_inflight")

    def test_lazy_allocation_failures_retry_without_consuming_operation_or_session_ids(self):
        self.run_case("allocation_retry")

    def test_real_esp_adapter_cleans_partial_allocations_and_pins_the_single_worker(self):
        result = subprocess.run([str(self.platform_binary)], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MONITOR_PASS platform_alloc", result.stdout)

    def test_stop_during_old_source_release_never_opens_the_candidate(self):
        self.run_case("stop_old_release")

    def test_failed_old_teardown_keeps_config_and_never_reopens_over_held_resources(self):
        self.run_case("old_release_failure")

    def test_uart_poll_error_closes_source_and_requires_an_explicit_retry(self):
        self.run_case("poll_failure")

    def test_logger_install_failure_never_claims_running_or_touches_uart(self):
        self.run_case("logger_failure")


if __name__ == "__main__":
    unittest.main()
