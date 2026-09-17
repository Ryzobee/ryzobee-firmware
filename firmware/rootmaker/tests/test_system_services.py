"""Host evidence for the real C owner loop, not Wi-Fi/FreeRTOS board evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "ryz_system_services"


class SystemServicesHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-system-services-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "system_services_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pthread", "-fsanitize=address,undefined", "-g"]
        for include in [ROOT / "tests/system_services_stubs", COMPONENT,
                        COMPONENT / "include",
                        ROOT / "components/ryz_provisioning/include",
                        ROOT / "components/ryz_time/include",
                        ROOT / "components/ryz_ota/include"]:
            command.extend(["-I", str(include)])
        command.extend([str(COMPONENT / "ryz_system_services.c"),
                        str(COMPONENT / "ryz_system_metrics.c"),
                        str(ROOT / "tests/system_services_test.c"),
                        "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SYSTEM_SERVICES_PASS " + name, result.stdout)

    def test_start_errors_retry_and_idempotence(self):
        self.run_case("startup")

    def test_concurrent_start_is_rejected_until_task_creation_finishes(self):
        self.run_case("concurrent_start")

    def test_independent_owner_and_clock_change_without_network_revision(self):
        self.run_case("independent")

    def test_snapshot_failures_close_admission_and_erase_invalid_data(self):
        self.run_case("fail_closed")

    def test_ntp_backoff_does_not_delay_offline_revocation(self):
        self.run_case("ntp_backoff")

    def test_init_retries_but_settled_boot_failure_does_not_start_radio_later(self):
        self.run_case("initial_retry")

    def test_reprovision_is_async_single_slot_and_exposes_operation_result(self):
        self.run_case("reprovision")

    def test_error_logs_are_rate_limited_per_stage(self):
        self.run_case("logging")

    def test_network_off_waits_for_ota_cleanup_and_holds_new_admission(self):
        self.run_case("network_pending")

    def test_cleanup_or_platform_failure_does_not_fabricate_radio_off(self):
        self.run_case("network_cleanup_failure")

    def test_explicit_off_overrides_failed_boot_start_without_harming_health_gate(self):
        self.run_case("network_overrides_retry")

    def test_setup_is_a_distinct_async_operation_waiting_for_real_ota_cleanup(self):
        self.run_case("network_setup")

    def test_setup_failure_and_unknown_action_never_fall_back_to_on_off_or_forget(self):
        self.run_case("network_setup_failures")

    def test_completed_network_state_is_same_cycle_and_retained_while_next_command_waits(self):
        self.run_case("network_completion_state")

    def test_failed_completion_snapshot_has_invalid_metadata_and_is_never_backfilled(self):
        self.run_case("network_completion_invalid_sample")

    def test_metrics_share_real_owner_one_second_cadence_and_copy_only_snapshot(self):
        self.run_case("metrics")

    def test_metrics_errors_counter_reset_clock_reversal_and_independent_recovery(self):
        self.run_case("metrics_errors")

    def test_metrics_block_does_not_hold_public_snapshot_lock(self):
        self.run_case("metrics_blocked")

    def test_metrics_use_independent_core_intervals_and_rebase_after_ipc_failure(self):
        self.run_case("metrics_skew")

    def test_boot_saved_sta_exact_deadline_and_one_shot_ap_fallback(self):
        self.run_case("boot_timeout")

    def test_boot_window_starts_after_slow_start_returns(self):
        self.run_case("boot_start_clock")

    def test_boot_off_ap_online_settle_without_wait_and_never_reenter_on_disconnect(self):
        for case in ("boot_off", "boot_ap", "boot_online"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_boot_failures_settle_explicitly_without_hidden_restart(self):
        for case in ("boot_failed", "boot_sync_failure", "boot_snapshot_failure", "boot_fallback_failure"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_boot_condition_recheck_can_preserve_just_established_sta(self):
        self.run_case("boot_late_online")

    def test_boot_fallback_waits_ota_cleanup_and_does_not_release_explicit_off_hold(self):
        for case in ("boot_ota", "boot_cleanup_failure", "boot_explicit_off"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_boot_network_io_does_not_block_snapshot_or_admit_overlapping_control(self):
        self.run_case("boot_fallback_blocked")

    def test_boot_ap_fast_browser_submit_keeps_its_own_connect_deadline(self):
        self.run_case("boot_portal_submit")


if __name__ == "__main__":
    unittest.main()
