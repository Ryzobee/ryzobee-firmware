"""Real BLE owner/storage/SDK Adapter; external radio/NVS/NPL test transports."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_ble"


class BleBackendTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ryz-ble-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "ble"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-fsanitize=address,undefined", "-DRYZ_BLE_HOST_TEST",
            "-I" + str(ROOT / "tests/ble_stubs"), "-I" + str(COMPONENT / "include"),
            "-I" + str(COMPONENT), str(COMPONENT / "ryz_ble.c"), str(COMPONENT / "ryz_ble_store.c"),
            str(ROOT / "tests/ble_backend_test.c"), "-o", str(cls.binary)], check=True, timeout=30)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RYZ_BLE_BACKEND_PASS", result.stdout)

    def test_boot_preserves_off_no_peer_and_timeout_persistent_policy(self):
        for case in ("boot_off", "boot_empty", "boot_timeout"):
            with self.subTest(case=case): self.case(case)

    def test_saved_peer_real_sdk_complete_then_encryption_sequence(self): self.case("restore")
    def test_numeric_comparison_and_verified_durable_bond_are_both_required(self):
        for case in ("pair", "no_numeric", "number"):
            with self.subTest(case=case): self.case(case)

    def test_cancel_reject_timeout_revoke_before_late_keys_or_encryption(self):
        for case in ("cancel", "reject", "pair_timeout"):
            with self.subTest(case=case): self.case(case)

    def test_pair_persistence_failures_remove_an_ambiguously_committed_new_bond(self):
        for case in ("save_fail", "save_unknown", "readback_fail"):
            with self.subTest(case=case): self.case(case)

    def test_irreversible_commit_rejects_cancel_admission(self): self.case("commit_gate")
    def test_forget_replace_wait_for_disconnect_and_durable_deletion(self):
        for case in ("forget", "replace", "forget_fail", "replace_fail"):
            with self.subTest(case=case): self.case(case)

    def test_explicit_off_preserves_bond_and_waits_for_physical_link_end(self): self.case("off")
    def test_failed_stop_never_claims_off(self): self.case("stop_fail")
    def test_foreign_peer_or_invalid_record_cannot_become_authenticated(self):
        for case in ("foreign", "bad_record"):
            with self.subTest(case=case): self.case(case)

    def test_synchronous_reject_callbacks_cannot_replace_the_accepted_off_target(self): self.case("sync_reject")
    def test_reset_before_first_sync_still_arms_pairing_timeout(self): self.case("reset_timeout")
    def test_unreadable_after_commit_attempts_cleanup_from_known_pre_pair_identity(self): self.case("unknown_cleanup")
    def test_reused_handle_cannot_accept_previous_session_authentication_events(self): self.case("reused_handle")
    def test_real_identity_resolved_event_binds_both_candidate_security_records(self): self.case("identity")
    def test_late_queued_connection_after_off_is_tracked_until_confirmed_shutdown(self): self.case("late_connect")
    def test_timer_failure_never_starts_an_unbounded_boot_pairing_window(self): self.case("timer_fail")

    def test_ancs_discovery_is_secure_and_never_subscribes_to_notifications(self): self.case("ancs")
    def test_ancs_service_changed_retries_missing_or_inflight_service(self):
        for case in ("ancs_changed", "ancs_dirty"):
            with self.subTest(case=case): self.case(case)
    def test_ancs_errors_and_timeout_do_not_invalidate_a_good_bond(self):
        for case in ("ancs_error", "ancs_timeout", "ancs_bad"):
            with self.subTest(case=case): self.case(case)
    def test_ancs_late_response_after_off_cannot_affect_reused_connection(self): self.case("ancs_late")
    def test_ancs_broadcast_encoding_failure_prevents_pairing_and_can_retry(self): self.case("ancs_adv_error")
    def test_ancs_uuid_without_notification_source_is_not_discovered(self): self.case("ancs_missing_source")
    def test_ancs_service_changed_authorization_error_is_not_a_pair_failure(self): self.case("ancs_watch_error")

    def test_hid_job_leases_taps_cleanup_and_stale_tokens(self): self.case("hid_lifecycle")
    def test_hid_pending_input_never_crosses_off_or_connection_epoch(self): self.case("hid_off_epoch")
    def test_hid_release_failure_disconnects_instead_of_leaving_a_held_key(self): self.case("hid_release_failure")
    def test_hid_scan_response_failure_never_starts_partial_advertising(self): self.case("hid_scan_response_failure")
    def test_hid_off_releases_before_terminate_and_retries_failed_termination(self): self.case("hid_stop_retry")
    def test_hid_unsubscribe_deletes_only_authenticated_peers_cccd_durably(self): self.case("hid_unsubscribe_persistence")


if __name__ == "__main__": unittest.main()
