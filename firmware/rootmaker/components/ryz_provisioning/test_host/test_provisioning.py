"""Host contract tests for the public Ryzobee provisioning seam."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class ProvisioningStateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.component = Path(__file__).resolve().parents[1]
        cls.temporary_directory = tempfile.TemporaryDirectory(
            prefix="ryzobee-provisioning-test-"
        )
        cls.binary = Path(cls.temporary_directory.name) / "provisioning-test"
        command = [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-pthread",
            "-I" + str(cls.component / "test_host/stubs"),
            "-I" + str(cls.component / "include"),
            "-I" + str(cls.component),
            str(cls.component / "test_host/provisioning_state_test.c"),
            str(cls.component / "test_host/fake_platform.c"),
            str(cls.component / "ryz_provisioning.c"),
            "-o",
            str(cls.binary),
        ]
        subprocess.run(command, check=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temporary_directory.cleanup()

    def run_case(self, name):
        completed = subprocess.run(
            [str(self.binary), name], text=True, capture_output=True, check=False, timeout=10
        )
        self.assertEqual(
            completed.returncode, 0, completed.stderr + completed.stdout
        )
        self.assertIn("RYZ_PROVISIONING_STATE_PASS", completed.stdout)

    def test_unconfigured_device_runs_portal_then_connects(self):
        self.run_case("portal")

    def test_boot_fallback_only_consumes_the_first_unsettled_saved_station_attempt(self):
        for variant in range(6):
            with self.subTest(variant=variant):
                self.run_case(f"boot-fallback-{variant}")

    def test_initial_ap_fast_submission_cannot_be_mistaken_for_saved_sta_boot_restore(self):
        self.run_case("boot-initial-ap-fast-submit")

    def test_explicit_setup_keeps_saved_network_and_ready_portal_is_idempotent(self):
        self.run_case("open-portal")

    def test_setup_from_off_reads_actual_saved_or_missing_identity(self):
        for name in ("open-without-start", "open-off-stored"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_setup_failures_stop_the_sequence_keep_credentials_and_allow_explicit_retry(self):
        for name in ("open-stop-failure", "open-load-failure", "open-start-failure", "open-address-failure"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_setup_fast_submit_cannot_be_overwritten_by_portal_ready(self):
        self.run_case("open-fast-submit")

    def test_setup_control_gate_rejects_concurrent_mutations_without_blocking_snapshot(self):
        self.run_case("open-concurrent")

    def test_link_diagnostics_require_matching_online_identity_and_monotonic_sample(self):
        self.run_case("link-binding")

    def test_connection_and_stop_transitions_revoke_all_link_diagnostics(self):
        self.run_case("link-revocation")

    def test_portal_start_result_cannot_overwrite_newer_http_or_worker_event(self):
        self.run_case("portal-publish-race")

    def test_reprovision_start_result_cannot_overwrite_newer_http_or_worker_event(self):
        self.run_case("reprovision-publish-race")

    def test_radio_toggle_preserves_credentials_and_off_intent_in_this_boot(self):
        self.run_case("power-cycle")

    def test_saved_off_boots_without_radio_or_credential_reads(self):
        self.run_case("pref-boot-off")

    def test_saved_on_boots_into_saved_station_without_rewriting_preference(self):
        self.run_case("pref-boot-on")

    def test_preference_read_failures_block_init_and_later_retry_reloads_value(self):
        self.run_case("pref-init-retry")

    def test_missing_key_defaults_on_and_first_explicit_on_is_saved_once(self):
        self.run_case("pref-missing")

    def test_failed_save_keeps_radio_state_and_releases_the_control_gate(self):
        self.run_case("pref-save-failure")

    def test_ambiguous_write_invalidates_the_cached_preference(self):
        self.run_case("pref-ambiguous")

    def test_runtime_failure_keeps_saved_preference_and_retry_still_operates_radio(self):
        self.run_case("pref-runtime-failure")

    def test_temporary_setup_and_legacy_forget_do_not_overwrite_switch(self):
        for name in ("pref-temporary-ap", "pref-temporary-forget"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_stop_failure_is_not_off_and_on_cannot_bypass_old_cleanup(self):
        self.run_case("stop-confirmation")

    def test_forget_waits_for_stop_and_never_hides_persistence_failure(self):
        self.run_case("forget-failure")

    def test_off_before_first_start_cannot_be_overridden_by_background_start(self):
        self.run_case("off-before-start")

    def test_stored_credentials_start_in_station_mode(self):
        self.run_case("stored")

    def test_reprovision_clears_credentials_and_reopens_portal(self):
        self.run_case("reprovision")

    def test_failures_are_visible_in_snapshot(self):
        self.run_case("failure")

    def test_portal_without_an_ipv4_address_is_not_ready(self):
        self.run_case("portal-address-failure")

    def test_platform_failure_clears_active_portal_address(self):
        self.run_case("portal-runtime-failure")

    def test_snapshot_is_coherent_during_event_updates(self):
        self.run_case("threadsafe")

    def test_stale_connect_command_does_not_cancel_new_pending_command(self):
        self.run_case("generation")

    def test_reconnect_policy_is_bounded_and_generation_safe(self):
        self.run_case("reconnect")

    def test_partial_initialization_has_retryable_reverse_rollback_plan(self):
        self.run_case("init-plan")

    def test_identity_failure_releases_platform_for_retry(self):
        self.run_case("init-retry")

    def test_start_is_idempotent_while_station_is_active(self):
        self.run_case("idempotent-start")

    def test_local_secret_uses_core_online_gate_and_clears_after_concurrent_stop(self):
        self.run_case("local-secret")


if __name__ == "__main__":
    unittest.main()
