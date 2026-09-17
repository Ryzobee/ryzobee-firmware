"""Real provisioning core + ESP HTTP/event/worker Adapter; no radio or sockets."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_provisioning"


class ProvisioningEspTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-provisioning-esp-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "provisioning_esp"
        json = Path(os.environ["IDF_PATH"]) / "components/json/cJSON"
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                 "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined",
                 "-DESP_ERR_INVALID_RESPONSE=0x108"]
        # Exercise the genuine compile-time getter guard without changing the
        # target configuration; only this isolated Host binary is affected.
        ipv6 = os.environ.get("RYZ_PROVISIONING_TEST_IPV6", "1")
        if ipv6 not in ("0", "1"):
            raise ValueError("RYZ_PROVISIONING_TEST_IPV6 must be 0 or 1")
        flags.append("-DCONFIG_LWIP_IPV6=" + ipv6)
        json_object = Path(cls.directory.name) / "json.o"
        subprocess.run(flags + ["-Wno-deprecated-declarations", "-I", str(json), "-c",
                               str(json / "cJSON.c"), "-o", str(json_object)], check=True, timeout=30)
        command = flags.copy()
        for include in [ROOT / "tests/provisioning_esp_stubs", COMPONENT, COMPONENT / "include", json]:
            command += ["-I", str(include)]
        command += [str(COMPONENT / "ryz_provisioning.c"), str(COMPONENT / "ryz_provisioning_esp.c"),
                    str(COMPONENT / "ryz_provisioning_portal.c"),
                    str(COMPONENT / "ryz_net_traffic.c"),
                    str(ROOT / "tests/provisioning_esp_test.c"), str(json_object), "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PROVISIONING_ESP_PASS " + name, result.stdout)

    def test_failed_http_stop_preserves_handle_for_retry_without_starting_station(self):
        self.run_case("http_stop_failure")

    def test_boot_directed_scan_admits_only_exact_ssid_and_releases_driver_records(self):
        for variant in range(12):
            with self.subTest(variant=variant):
                self.run_case(f"boot_scan_{variant}")

    def test_boot_fallback_preserves_records_starts_protected_ap_and_revokes_stale_events(self):
        self.run_case("boot_fallback_ap")
        self.run_case("boot_fallback_start_failed")

    def test_boot_fallback_checks_live_sta_before_queued_ip_and_preserves_online_and_normal_retry(self):
        self.run_case("boot_fallback_live")
        self.run_case("boot_fallback_online")

    def test_boot_fallback_preserves_saved_off_and_any_explicit_radio_control(self):
        for variant in range(5):
            with self.subTest(variant=variant):
                self.run_case(f"boot_fallback_superseded_{variant}")

    def test_boot_fallback_does_not_treat_read_stop_or_ap_start_failure_as_ready(self):
        for stage in range(6):
            with self.subTest(stage=stage):
                self.run_case(f"boot_fallback_failure_{stage}")

    def test_boot_fallback_does_not_overwrite_a_new_http_submission(self):
        self.run_case("boot_fallback_fast_post")

    def test_boot_fallback_does_not_accept_cached_foreign_or_zero_station_ip(self):
        for variant in range(3):
            with self.subTest(variant=variant):
                self.run_case(f"boot_fallback_unverified_{variant}")

    def test_first_ap_fast_http_connecting_never_acquires_boot_fallback_admission(self):
        self.run_case("boot_initial_fast_post")

    def test_once_verified_online_even_a_drop_before_the_boot_poll_keeps_normal_reconnect(self):
        self.run_case("boot_online_dropped")

    def test_enabled_preference_platform_load_and_save_use_their_own_committed_key(self):
        self.run_case("enabled_platform")

    def test_persisted_off_boot_does_not_start_radio_or_rewrite_known_preference(self):
        self.run_case("enabled_boot_off")

    def test_preference_load_rejects_absent_namespace_bad_value_type_and_sdk_errors(self):
        self.run_case("enabled_load_errors")

    def test_preference_save_failures_close_handles_and_do_not_touch_other_keys(self):
        self.run_case("enabled_save_errors")

    def test_corrupt_or_unreadable_preference_refuses_real_initialization_and_can_retry(self):
        for case in ["enabled_bad2", "enabled_bad255", "enabled_badtype",
                     "enabled_get_failure", "enabled_open_failure"]:
            with self.subTest(case=case):
                self.run_case(case)

    def test_on_and_off_save_failures_preserve_current_network_then_explicit_retry_works(self):
        for direction in ["on", "off"]:
            for stage in ["open", "set", "commit"]:
                with self.subTest(direction=direction, stage=stage):
                    self.run_case(f"enabled_{direction}_{stage}")

    def test_missing_preference_defaults_on_but_only_explicit_on_saves_it(self):
        self.run_case("enabled_first_on")

    def test_commit_then_error_invalidates_old_preference_cache_without_changing_radio(self):
        self.run_case("enabled_commit_unknown")

    def test_radio_failure_keeps_committed_preference_and_same_value_retry_does_not_rewrite(self):
        for case in ["enabled_radio_on", "enabled_radio_off"]:
            with self.subTest(case=case):
                self.run_case(case)

    def test_open_portal_and_reprovision_temporarily_enable_without_overwriting_saved_off(self):
        self.run_case("enabled_temporary")

    def test_stop_cancels_delayed_worker_but_preserves_credentials_for_explicit_on(self):
        self.run_case("stop_credentials")

    def test_registered_http_handler_try_lock_avoids_server_stop_deadlock(self):
        self.run_case("http_try_lock")

    def test_old_mailbox_and_late_sdk_events_cannot_publish_an_old_connection(self):
        self.run_case("stale_events")

    def test_failed_http_stop_is_not_off_and_forget_cannot_erase_credentials(self):
        self.run_case("stop_http")

    def test_failed_dns_stop_retains_the_original_handle_for_explicit_retry(self):
        self.run_case("stop_dns")

    def test_failed_radio_stop_cannot_report_off_or_start_another_session(self):
        self.run_case("stop_wifi")

    def test_registered_post_and_forget_commit_failures_do_not_fabricate_persistence(self):
        self.run_case("persistence")

    def test_unverified_ip_arriving_at_retry_deadline_cannot_consume_retry(self):
        self.run_case("retry_late_ip")

    def test_ignored_coalesced_ip_and_foreign_disconnect_preserve_current_retry(self):
        self.run_case("retry_ignored_batch")

    def test_link_uses_periodic_worker_sampling_and_public_snapshot_is_copy_only(self):
        self.run_case("link_periodic")

    def test_sta_traffic_uses_actual_wrappers_periodic_sampling_and_copy_only_snapshot(self):
        self.run_case("traffic_periodic")

    def test_ap_and_unverified_station_traffic_cannot_create_or_pollute_sta_measurement(self):
        self.run_case("traffic_ap")

    def test_traffic_disconnect_reconnect_and_failed_stop_do_not_reuse_old_counters(self):
        self.run_case("traffic_restart")

    def test_traffic_roam_and_raw_disconnect_during_sampling_use_fresh_epochs(self):
        self.run_case("traffic_raw")

    def test_sdk_or_mid_sample_identity_failure_revokes_link_without_changing_online(self):
        self.run_case("link_failures")

    def test_disconnect_failed_stop_and_new_station_epoch_revoke_link_binding(self):
        self.run_case("link_restart")

    def test_local_secret_try_lock_clear_output_and_no_snapshot_http_or_sdk_reads(self):
        self.run_case("secret_api")

    def test_local_secret_epoch_changes_on_roam_failure_expiry_disconnect_and_config(self):
        self.run_case("secret_epochs")

    def test_local_secret_open_and_maximum_length_passwords(self):
        self.run_case("secret_open")

    def test_raw_disconnect_revokes_before_worker_and_during_sampling_without_changing_retry(self):
        self.run_case("secret_event")

    def test_open_portal_preserves_saved_credentials_and_healthy_portal_is_idempotent(self):
        self.run_case("open_preserves")

    def test_open_portal_ignores_old_and_late_station_events_and_revokes_retry_ticket(self):
        self.run_case("open_stale_events")

    def test_open_portal_revokes_queued_http_connection_without_rewriting_credentials(self):
        self.run_case("open_queued")

    def test_open_portal_revokes_worker_delayed_http_connection_without_rewriting_credentials(self):
        self.run_case("open_delayed")

    def test_open_portal_http_stop_failure_retains_handle_and_never_starts_another_ap(self):
        self.run_case("open_stop_http")

    def test_open_portal_dns_stop_failure_retains_handle_and_never_starts_another_ap(self):
        self.run_case("open_stop_dns")

    def test_open_portal_wifi_stop_failure_retains_radio_and_never_starts_another_ap(self):
        self.run_case("open_stop_wifi")

    def test_open_portal_wifi_start_failure_preserves_credentials_and_reports_error(self):
        self.run_case("open_start_wifi")

    def test_open_portal_http_start_failure_preserves_credentials_and_reports_error(self):
        self.run_case("open_start_http")

    def test_open_portal_dns_start_failure_preserves_credentials_and_reports_error(self):
        self.run_case("open_start_dns")

    def test_open_portal_fast_registered_http_submission_is_not_overwritten_by_ready(self):
        self.run_case("open_fast_post")


if __name__ == "__main__":
    unittest.main()
