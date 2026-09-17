#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_platform.h"
#include "ryz_provisioning.h"
#include "ryz_provisioning_local_secret.h"
#include "ryz_provisioning_generation.h"
#include "ryz_provisioning_init_plan.h"

static ryz_provisioning_snapshot_t snapshot(void)
{
    ryz_provisioning_snapshot_t value;
    assert(ryz_provisioning_get_snapshot(&value) == ESP_OK);
    return value;
}

static void test_boot_fallback(unsigned variant)
{
    fake_platform_reset();
    fake_platform_store_credentials("SAVED", "test-pass8");
    fake_platform_set_enabled_record(ESP_OK, variant != 1);
    if (variant == 4) fake_platform_set_start_sta_result(ESP_FAIL);
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == (variant == 4 ? ESP_FAIL : ESP_OK));
    assert(snapshot().boot_restore_pending == (variant != 1));
    if (variant == 2) fake_platform_got_ip("SAVED", "10.0.0.1");
    if (variant == 3) assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    if (variant == 5) fake_platform_set_stop_result(ESP_FAIL);
    const ryz_provisioning_snapshot_t before = snapshot();
    assert(before.boot_restore_pending == (variant == 0 || variant == 4 || variant == 5));
    assert(ryz_provisioning_boot_fallback() == (variant == 5 ? ESP_FAIL : ESP_OK));
    if (variant == 1 || variant == 2 || variant == 3) {
        assert(snapshot().revision == before.revision);
        assert(fake_platform_stop_count() == 0 && fake_platform_start_portal_count() == 0);
    } else {
        assert(snapshot().enabled && snapshot().credentials_stored);
        assert(snapshot().state == (variant == 5 ? RYZ_PROVISIONING_FAILED : RYZ_PROVISIONING_AP_READY));
        assert(fake_platform_stop_count() == 1);
        assert(fake_platform_start_portal_count() == (variant == 5 ? 0U : 1U));
    }
    ryz_provisioning_snapshot_t settled = snapshot();
    assert(!settled.boot_restore_pending);
    assert(ryz_provisioning_boot_fallback() == ESP_OK);
    assert(snapshot().revision == settled.revision); /* One-shot even on error. */
    assert(fake_platform_credentials_exist() && fake_platform_clear_count() == 0);
    assert(fake_platform_enabled_save_count() == 0 && fake_platform_load_count() == (variant == 1 ? 0U : 1U));
}

static void test_preference_boot(bool enabled)
{
    fake_platform_reset();
    fake_platform_store_credentials("SAVED", "test-pass8");
    fake_platform_set_enabled_record(ESP_OK, enabled);
    assert(ryz_provisioning_init() == ESP_OK);
    assert(snapshot().enabled == enabled);
    assert(snapshot().state == (enabled ? RYZ_PROVISIONING_UNCONFIGURED : RYZ_PROVISIONING_OFF));
    assert(snapshot().stop_confirmed);
    assert(ryz_provisioning_init() == ESP_OK);
    assert(fake_platform_enabled_load_count() == 1);
    for (int i = 0; i < 4; ++i) assert(ryz_provisioning_start() == ESP_OK);
    assert(fake_platform_start_sta_count() == (enabled ? 1U : 0U));
    assert(fake_platform_saved_scan_count() == (enabled ? 1U : 0U));
    assert(snapshot().boot_phase == (enabled ? RYZ_PROVISIONING_BOOT_CONNECTING : RYZ_PROVISIONING_BOOT_OFF));
    assert(fake_platform_start_portal_count() == 0 && fake_platform_stop_count() == 0);
    assert(fake_platform_enabled_save_count() == 0 && fake_platform_credentials_exist());
}

static void test_preference_init_retry(void)
{
    fake_platform_reset();
    fake_platform_set_enabled_record(ESP_FAIL, true);
    assert(ryz_provisioning_init() == ESP_FAIL);
    ryz_provisioning_snapshot_t value;
    assert(ryz_provisioning_get_snapshot(&value) == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_start() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_set_enabled(true) == ESP_ERR_INVALID_STATE);
    assert(fake_platform_deinit_count() == 1);
    fake_platform_set_enabled_record(ESP_OK, true);
    fake_platform_set_ap_identity_result(ESP_FAIL);
    assert(ryz_provisioning_init() == ESP_FAIL);
    assert(fake_platform_deinit_count() == 2);
    /* A later successful init must re-read, not reuse the aborted ON value. */
    fake_platform_set_enabled_record(ESP_OK, false);
    fake_platform_set_ap_identity_result(ESP_OK);
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_OFF && !snapshot().enabled);
    assert(fake_platform_enabled_load_count() == 3 && fake_platform_enabled_save_count() == 0);
    assert(fake_platform_start_sta_count() == 0 && fake_platform_start_portal_count() == 0);
}

static void test_missing_preference_first_explicit_on(void)
{
    fake_platform_reset();
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    assert(snapshot().enabled && snapshot().state == RYZ_PROVISIONING_AP_READY);
    assert(fake_platform_enabled_save_count() == 0);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(fake_platform_enabled_save_count() == 1 && fake_platform_saved_enabled());
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(fake_platform_enabled_save_count() == 1 && fake_platform_start_portal_count() == 1);
}

static ryz_provisioning_snapshot_t before_preference_write;
static void preference_write_hook(void)
{
    ryz_provisioning_snapshot_t current = snapshot();
    assert(current.revision == before_preference_write.revision);
    assert(current.state == before_preference_write.state);
    assert(current.enabled == before_preference_write.enabled);
    assert(ryz_provisioning_set_enabled(false) == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_start() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_open_portal() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_request_reprovision() == ESP_ERR_INVALID_STATE);
}

static void test_preference_save_failure(bool ambiguous)
{
    fake_platform_reset();
    fake_platform_set_enabled_record(ESP_OK, true);
    fake_platform_store_credentials("KEEP", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    fake_platform_got_ip("KEEP", "10.0.0.1");
    before_preference_write = snapshot();
    fake_platform_set_save_enabled_hook(preference_write_hook);
    fake_platform_set_save_enabled_result(ESP_FAIL, ambiguous);
    assert(ryz_provisioning_set_enabled(false) == ESP_FAIL);
    assert(snapshot().revision == before_preference_write.revision);
    assert(snapshot().state == RYZ_PROVISIONING_ONLINE && snapshot().enabled);
    assert(fake_platform_stop_count() == 0 && fake_platform_enabled_save_count() == 1);
    assert(fake_platform_saved_enabled() == !ambiguous);
    fake_platform_set_save_enabled_result(ESP_OK, false);
    /* The old cached ON is invalid after an ambiguous failed OFF write. */
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(fake_platform_enabled_save_count() == 2 && fake_platform_saved_enabled());
    assert(fake_platform_start_sta_count() == 1 && fake_platform_stop_count() == 0);
    fake_platform_set_save_enabled_hook(NULL);
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    assert(!fake_platform_saved_enabled() && snapshot().state == RYZ_PROVISIONING_OFF);
    assert(fake_platform_credentials_exist() && fake_platform_clear_count() == 0);
}

static void test_preference_runtime_failure_does_not_rollback(void)
{
    fake_platform_reset();
    fake_platform_set_enabled_record(ESP_OK, true);
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    fake_platform_set_stop_result(ESP_FAIL);
    assert(ryz_provisioning_set_enabled(false) == ESP_FAIL);
    assert(!fake_platform_saved_enabled() && fake_platform_enabled_save_count() == 1);
    assert(snapshot().state == RYZ_PROVISIONING_FAILED && !snapshot().stop_confirmed);
    fake_platform_set_stop_result(ESP_OK);
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    assert(fake_platform_enabled_save_count() == 1 && fake_platform_stop_count() == 2);
    fake_platform_set_start_portal_result(ESP_FAIL);
    assert(ryz_provisioning_set_enabled(true) == ESP_FAIL);
    assert(fake_platform_saved_enabled() && fake_platform_enabled_save_count() == 2);
    fake_platform_set_start_portal_result(ESP_OK);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY);
    assert(fake_platform_enabled_save_count() == 2);
}

static void test_temporary_ap_does_not_save_preference(bool forget)
{
    fake_platform_reset();
    fake_platform_set_enabled_record(ESP_OK, false);
    fake_platform_store_credentials("SAVED", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    assert((forget ? ryz_provisioning_request_reprovision() : ryz_provisioning_open_portal()) == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY && snapshot().enabled);
    assert(!fake_platform_saved_enabled() && fake_platform_enabled_save_count() == 0);
    /* Cached OFF skips only NVS, never the actual temporary AP shutdown. */
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_OFF && !snapshot().enabled);
    assert(fake_platform_stop_count() == 1 && fake_platform_enabled_save_count() == 0);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(fake_platform_saved_enabled() && fake_platform_enabled_save_count() == 1);
}

static void test_open_portal_keeps_saved_network(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("KEEP-NET", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    fake_platform_got_ip("KEEP-NET", "10.2.3.4");
    assert(ryz_provisioning_open_portal() == ESP_OK);
    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_AP_READY && value.enabled && value.portal_active);
    assert(!value.stop_confirmed && value.credentials_stored && !value.ipv4[0]);
    assert(!strcmp(value.sta_ssid, "KEEP-NET") && !strcmp(value.portal_ip, "192.168.4.1"));
    assert(fake_platform_stop_count() == 1 && fake_platform_load_count() == 2);
    assert(fake_platform_start_portal_count() == 1 && fake_platform_clear_count() == 0);
    assert(fake_platform_credentials_exist() && fake_platform_portal_start_observed_clean_snapshot());
    /* A genuine ready AP is not stopped/restarted by a second setup request. */
    assert(ryz_provisioning_open_portal() == ESP_OK);
    assert(snapshot().revision == value.revision && fake_platform_stop_count() == 1);
    assert(fake_platform_start_portal_count() == 1 && fake_platform_load_count() == 2);
}

static void test_power_cycle_keeps_credentials(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("LAB", "test-pass8");
    assert(ryz_provisioning_set_enabled(false) == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    fake_platform_got_ip("LAB", "10.2.3.4");
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    ryz_provisioning_snapshot_t off = snapshot();
    assert(off.state == RYZ_PROVISIONING_OFF && !off.enabled && off.stop_confirmed);
    assert(off.credentials_stored && fake_platform_credentials_exist());
    assert(!off.ipv4[0] && !off.portal_active && !off.portal_ip[0]);
    assert(ryz_provisioning_start() == ESP_OK); /* Background cannot undo OFF. */
    fake_platform_got_ip("OLD", "10.9.9.9");
    fake_platform_disconnect(3);
    assert(snapshot().revision == off.revision);
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    assert(fake_platform_stop_count() == 1);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(fake_platform_start_sta_count() == 2);
    assert(snapshot().enabled && snapshot().state == RYZ_PROVISIONING_CONNECTING);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(fake_platform_start_sta_count() == 2);
}

static void stop_hook(void)
{
    ryz_provisioning_snapshot_t before = snapshot();
    assert(before.state == RYZ_PROVISIONING_STOPPING && !before.enabled);
    assert(!before.stop_confirmed);
    fake_platform_got_ip("LATE", "1.2.3.4");
    assert(snapshot().revision == before.revision);
    assert(ryz_provisioning_set_enabled(true) == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_start() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_request_reprovision() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_open_portal() == ESP_ERR_INVALID_STATE);
}

static void test_stop_failure_requires_confirmation(void)
{
    fake_platform_reset();
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    fake_platform_set_stop_hook(stop_hook);
    fake_platform_set_stop_result(ESP_FAIL);
    assert(ryz_provisioning_set_enabled(false) == ESP_FAIL);
    ryz_provisioning_snapshot_t failed = snapshot();
    assert(failed.state == RYZ_PROVISIONING_FAILED && !failed.stop_confirmed);
    assert(!failed.enabled && failed.last_error == ESP_FAIL);
    assert(failed.portal_active); /* Last-known handle, not a fabricated OFF. */
    assert(ryz_provisioning_set_enabled(true) == ESP_FAIL);
    assert(fake_platform_stop_count() == 2 && !snapshot().enabled);
    fake_platform_set_stop_result(ESP_OK);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY && snapshot().enabled);
    assert(!snapshot().stop_confirmed && fake_platform_stop_count() == 3);
}

static void test_forget_failure_preserves_saved_identity(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("KEEP", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    fake_platform_set_stop_result(ESP_FAIL);
    assert(ryz_provisioning_request_reprovision() == ESP_FAIL);
    assert(fake_platform_credentials_exist() && snapshot().credentials_stored);
    fake_platform_set_stop_result(ESP_OK);
    fake_platform_set_clear_result(ESP_FAIL);
    assert(ryz_provisioning_request_reprovision() == ESP_FAIL);
    assert(snapshot().stop_confirmed && !snapshot().enabled);
    assert(fake_platform_credentials_exist() && snapshot().credentials_stored);
    fake_platform_set_clear_result(ESP_OK);
    assert(ryz_provisioning_request_reprovision() == ESP_OK);
    assert(!fake_platform_credentials_exist() && !snapshot().credentials_stored);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY && snapshot().enabled);
}

static void test_disable_before_first_start(void)
{
    fake_platform_reset();
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_OFF);
    assert(ryz_provisioning_start() == ESP_OK);
    assert(fake_platform_stop_count() == 0 && fake_platform_start_sta_count() == 0);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY);
}

static void test_unconfigured_portal_to_online(void)
{
    fake_platform_reset();
    fake_platform_set_portal_ip("172.23.4.1");
    assert(ryz_provisioning_init() == ESP_OK);

    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_UNCONFIGURED);
    assert(strcmp(value.ap_ssid, "RYZOBEE-12AB") == 0);
    assert(strlen(value.ap_password) >= 8);
    assert(!value.credentials_stored);
    assert(value.portal_ip[0] == '\0');

    assert(ryz_provisioning_start() == ESP_OK);
    value = snapshot();
    assert(value.state == RYZ_PROVISIONING_AP_READY);
    assert(value.portal_active);
    assert(strcmp(value.portal_ip, "172.23.4.1") == 0);
    assert(value.ipv4[0] == '\0');

    fake_platform_submit_credentials("LAB-NET", "correct-horse");
    value = snapshot();
    assert(value.state == RYZ_PROVISIONING_CREDENTIALS_RECEIVED);
    assert(value.credentials_stored);
    assert(value.portal_active);
    assert(strcmp(value.portal_ip, "172.23.4.1") == 0);
    assert(strcmp(value.sta_ssid, "LAB-NET") == 0);

    fake_platform_begin_connecting();
    value = snapshot();
    assert(value.state == RYZ_PROVISIONING_CONNECTING);
    assert(!value.portal_active);
    assert(value.portal_ip[0] == '\0');

    fake_platform_got_ip("LAB-NET", "192.168.10.24");
    value = snapshot();
    assert(value.state == RYZ_PROVISIONING_ONLINE);
    assert(value.portal_ip[0] == '\0');
    assert(strcmp(value.ipv4, "192.168.10.24") == 0);
}

static void test_stored_credentials_start_in_station_mode(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("OFFICE", "eight888");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);

    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_CONNECTING);
    assert(value.credentials_stored);
    assert(strcmp(value.sta_ssid, "OFFICE") == 0);
    assert(!value.portal_active);
    assert(value.portal_ip[0] == '\0');

    fake_platform_got_ip("OFFICE", "10.0.0.8");
    assert(strcmp(snapshot().ipv4, "10.0.0.8") == 0);
    fake_platform_begin_connecting();
    value = snapshot();
    assert(value.state == RYZ_PROVISIONING_CONNECTING);
    assert(value.ipv4[0] == '\0');
}

static void test_reprovision_clears_credentials_and_reopens_portal(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("OLD-NET", "old-pass8");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    fake_platform_got_ip("OLD-NET", "10.0.0.9");
    fake_platform_set_portal_ip("172.31.9.1");

    assert(ryz_provisioning_request_reprovision() == ESP_OK);
    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_AP_READY);
    assert(value.portal_active);
    assert(strcmp(value.portal_ip, "172.31.9.1") == 0);
    assert(!value.credentials_stored);
    assert(value.sta_ssid[0] == '\0');
    assert(value.ipv4[0] == '\0');
    assert(!fake_platform_credentials_exist());
    assert(fake_platform_portal_start_observed_clean_snapshot());
}

static void test_start_failures_are_observable(void)
{
    fake_platform_reset();
    fake_platform_set_start_portal_result(0x7001);
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == 0x7001);

    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_FAILED);
    assert(value.last_error == 0x7001);
    assert(value.portal_ip[0] == '\0');
}

static void test_platform_failure_clears_active_portal_address(void)
{
    fake_platform_reset();
    fake_platform_set_portal_ip("10.42.0.1");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    assert(strcmp(snapshot().portal_ip, "10.42.0.1") == 0);

    fake_platform_disconnect(77);
    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_FAILED);
    assert(!value.portal_active);
    assert(value.portal_ip[0] == '\0');
}

static void test_portal_without_an_ipv4_address_is_not_ready(void)
{
    fake_platform_reset();
    fake_platform_set_portal_ip("");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_FAIL);

    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_FAILED);
    assert(!value.portal_active);
    assert(value.portal_ip[0] == '\0');
}

static void *emit_many_events(void *argument)
{
    (void)argument;
    for (int index = 0; index < 20000; ++index) {
        fake_platform_got_ip((index & 1) ? "A" : "NETWORK-WITH-LONG-NAME",
                             (index & 1) ? "1.1.1.1" : "192.168.255.254");
    }
    return NULL;
}

static void test_snapshot_is_coherent_during_event_updates(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("BOOT", "password8");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);

    pthread_t writer;
    assert(pthread_create(&writer, NULL, emit_many_events, NULL) == 0);
    for (int index = 0; index < 20000; ++index) {
        ryz_provisioning_snapshot_t value = snapshot();
        assert(value.revision > 0);
        assert(value.state >= RYZ_PROVISIONING_UNCONFIGURED);
        assert(value.state <= RYZ_PROVISIONING_FAILED);
        assert(memchr(value.sta_ssid, '\0', sizeof(value.sta_ssid)) != NULL);
        assert(memchr(value.ipv4, '\0', sizeof(value.ipv4)) != NULL);
        if (value.state == RYZ_PROVISIONING_ONLINE) {
            bool short_value = strcmp(value.sta_ssid, "A") == 0 &&
                               strcmp(value.ipv4, "1.1.1.1") == 0;
            bool long_value = strcmp(value.sta_ssid, "NETWORK-WITH-LONG-NAME") == 0 &&
                              strcmp(value.ipv4, "192.168.255.254") == 0;
            assert(short_value || long_value);
        }
    }
    assert(pthread_join(writer, NULL) == 0);
}

static void test_stale_connect_command_does_not_cancel_new_pending_command(void)
{
    ryz_provisioning_generation_state_t state = {
        .generation = 12,
        .connect_pending = true,
    };

    assert(!ryz_provisioning_claim_connect_command(&state, 11));
    assert(state.generation == 12);
    assert(state.connect_pending);

    assert(ryz_provisioning_claim_connect_command(&state, 12));
    assert(state.generation == 13);
    assert(!state.connect_pending);

    state.generation = 22;
    state.connect_pending = true;
    assert(!ryz_provisioning_release_connect_command(&state, true, 21));
    assert(state.generation == 22);
    assert(state.connect_pending);
    assert(ryz_provisioning_release_connect_command(&state, true, 22));
    assert(!state.connect_pending);
}

static void test_reconnect_policy_is_bounded_and_backoff_increases(void)
{
    ryz_provisioning_generation_state_t state = {
        .generation = 40,
    };
    ryz_provisioning_reconnect_ticket_t ticket = {0};

    for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
        assert(ryz_provisioning_schedule_reconnect(
                   &state, true, false, &ticket) ==
               RYZ_PROVISIONING_RECONNECT_RETRY);
        assert(ticket.generation == (uint32_t)(39 + attempt));
        assert(ticket.attempt == attempt);
        assert(ticket.delay_ms == (uint32_t)attempt * 500U);
        assert(state.reconnect_pending);

        ryz_provisioning_reconnect_ticket_t duplicate = {0};
        assert(ryz_provisioning_schedule_reconnect(
                   &state, true, false, &duplicate) ==
               RYZ_PROVISIONING_RECONNECT_IGNORE);
        assert(state.reconnect_attempts == attempt);

        assert(ryz_provisioning_claim_reconnect(&state, true, &ticket));
        assert(!state.reconnect_pending);
    }

    uint32_t generation_before_exhaustion = state.generation;
    assert(ryz_provisioning_schedule_reconnect(
               &state, true, false, &ticket) ==
           RYZ_PROVISIONING_RECONNECT_FAILED_EXHAUSTED);
    assert(state.generation == generation_before_exhaustion + 1);
    assert(state.reconnect_attempts == 0);
    assert(!state.reconnect_pending);
}

static void test_auth_failure_and_mode_switch_cancel_reconnects(void)
{
    ryz_provisioning_generation_state_t state = {
        .generation = 90,
    };
    ryz_provisioning_reconnect_ticket_t ticket = {0};

    assert(ryz_provisioning_schedule_reconnect(
               &state, true, true, &ticket) ==
           RYZ_PROVISIONING_RECONNECT_FAILED_CREDENTIALS);
    assert(state.generation == 91);
    assert(!state.reconnect_pending);

    assert(ryz_provisioning_schedule_reconnect(
               &state, true, false, &ticket) ==
           RYZ_PROVISIONING_RECONNECT_RETRY);
    ryz_provisioning_begin_generation(&state);
    assert(!ryz_provisioning_claim_reconnect(&state, false, &ticket));
    assert(!state.reconnect_pending);

    ryz_provisioning_generation_state_t before = state;
    assert(ryz_provisioning_schedule_reconnect(
               &state, false, true, &ticket) ==
           RYZ_PROVISIONING_RECONNECT_IGNORE);
    assert(memcmp(&state, &before, sizeof(state)) == 0);

    assert(ryz_provisioning_network_event_is_current(
        &state, true, state.generation));
    assert(!ryz_provisioning_network_event_is_current(
        &state, true, state.generation - 1));
    assert(!ryz_provisioning_network_event_is_current(
        &state, false, state.generation));
}

static void test_online_keeps_station_epoch_for_following_disconnect(void)
{
    ryz_provisioning_generation_state_t state = {
        .generation = 120,
    };
    ryz_provisioning_reconnect_ticket_t ticket = {0};

    assert(ryz_provisioning_schedule_reconnect(
               &state, true, false, &ticket) ==
           RYZ_PROVISIONING_RECONNECT_RETRY);
    assert(state.reconnect_pending);

    ryz_provisioning_mark_online(&state);
    assert(state.generation == 120);
    assert(state.reconnect_attempts == 0);
    assert(!state.reconnect_pending);

    /* A disconnect queued immediately after got-IP belongs to the same STA
     * epoch and must remain actionable rather than becoming stale. */
    assert(ryz_provisioning_network_event_is_current(&state, true, 120));

    /* When GOT_IP and a following DISCONNECTED are coalesced into one worker
     * batch, the observed recovery resets the old outage before scheduling
     * the final disconnected level. */
    assert(ryz_provisioning_schedule_reconnect(
               &state, true, false, &ticket) ==
           RYZ_PROVISIONING_RECONNECT_RETRY);
    ryz_provisioning_reconnect_ticket_t recovered_ticket = {0};
    assert(ryz_provisioning_handle_disconnect(
               &state, true, true, false, &recovered_ticket) ==
           RYZ_PROVISIONING_RECONNECT_RETRY);
    assert(recovered_ticket.generation == 120);
    assert(recovered_ticket.attempt == 1);
    assert(recovered_ticket.delay_ms == 500U);
}

static void test_pending_online_observation_wins_reconnect_deadline(void)
{
    ryz_provisioning_generation_state_t state = {
        .generation = 200,
    };
    ryz_provisioning_reconnect_ticket_t ticket = {0};
    assert(ryz_provisioning_schedule_reconnect(
               &state, true, false, &ticket) ==
           RYZ_PROVISIONING_RECONNECT_RETRY);

    ryz_provisioning_generation_state_t before = state;
    assert(!ryz_provisioning_claim_reconnect_unless_online(
        &state, true, &ticket, true, ticket.generation));
    assert(memcmp(&state, &before, sizeof(state)) == 0);

    assert(ryz_provisioning_claim_reconnect_unless_online(
        &state, true, &ticket, false, 0));
    assert(state.generation == 201);
    assert(!state.reconnect_pending);
}

static void test_partial_initialization_rolls_back_only_owned_resources(void)
{
    ryz_provisioning_init_plan_t plan = {0};
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_EVENT_LOOP);
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_AP_NETIF);
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_WIFI);
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_NETWORK_MUTEX);
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_CONNECT_TASK);
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_IP_HANDLER);

    const ryz_provisioning_init_resource_t expected[] = {
        RYZ_PROVISIONING_INIT_IP_HANDLER,
        RYZ_PROVISIONING_INIT_CONNECT_TASK,
        RYZ_PROVISIONING_INIT_NETWORK_MUTEX,
        RYZ_PROVISIONING_INIT_WIFI,
        RYZ_PROVISIONING_INIT_AP_NETIF,
        RYZ_PROVISIONING_INIT_EVENT_LOOP,
    };
    ryz_provisioning_init_resource_t actual[RYZ_PROVISIONING_INIT_RESOURCE_COUNT];
    size_t count = ryz_provisioning_init_take_rollback_plan(
        &plan, actual, RYZ_PROVISIONING_INIT_RESOURCE_COUNT);
    assert(count == sizeof(expected) / sizeof(expected[0]));
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
    assert(plan.owned == 0);

    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_CONNECT_QUEUE);
    assert(plan.owned != 0);
}

static void test_identity_failure_rolls_back_platform_and_init_can_retry(void)
{
    fake_platform_reset();
    fake_platform_set_ap_identity_result(0x7101);
    assert(ryz_provisioning_init() == 0x7101);
    assert(fake_platform_init_count() == 1);
    assert(fake_platform_deinit_count() == 1);

    fake_platform_set_ap_identity_result(ESP_OK);
    assert(ryz_provisioning_init() == ESP_OK);
    assert(fake_platform_init_count() == 2);
    assert(fake_platform_deinit_count() == 1);
    assert(snapshot().state == RYZ_PROVISIONING_UNCONFIGURED);
}

static void test_start_is_idempotent_while_station_is_active(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("OFFICE", "eight888");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    assert(fake_platform_start_sta_count() == 1);

    fake_platform_got_ip("OFFICE", "10.0.0.2");
    assert(snapshot().state == RYZ_PROVISIONING_ONLINE);
    assert(ryz_provisioning_start() == ESP_OK);
    assert(fake_platform_start_sta_count() == 1);
    assert(snapshot().state == RYZ_PROVISIONING_ONLINE);
}

static void submit_before_start_returns(void)
{
    fake_platform_submit_credentials("NEW-NETWORK", "eight888");
    fake_platform_begin_connecting();
    fake_platform_got_ip("NEW-NETWORK", "10.1.2.3");
}

static void submit_connecting_before_start_returns(void)
{
    fake_platform_submit_credentials("NEW-NETWORK", "eight888");
    fake_platform_begin_connecting();
}

static void test_initial_ap_submission_is_not_boot_restore(void)
{
    fake_platform_reset();
    assert(ryz_provisioning_init() == ESP_OK);
    fake_platform_set_portal_start_hook(submit_connecting_before_start_returns);
    assert(ryz_provisioning_start() == ESP_OK);
    ryz_provisioning_snapshot_t before = snapshot();
    assert(before.state == RYZ_PROVISIONING_CONNECTING && before.credentials_stored);
    assert(!before.boot_restore_pending);
    assert(ryz_provisioning_boot_fallback() == ESP_OK);
    assert(snapshot().revision == before.revision && fake_platform_stop_count() == 0);
    assert(fake_platform_start_portal_count() == 1 && fake_platform_start_sta_count() == 0);
}

static void test_portal_start_cannot_overwrite_newer_event(bool reprovision)
{
    fake_platform_reset();
    if (reprovision) fake_platform_store_credentials("OLD-NETWORK", "eight888");
    assert(ryz_provisioning_init() == ESP_OK);
    if (reprovision) assert(ryz_provisioning_start() == ESP_OK);
    /* The ESP Adapter releases its network lock before returning. A registered
     * HTTP/worker event can therefore advance the core before AP_READY publish. */
    fake_platform_set_portal_start_hook(submit_before_start_returns);
    assert((reprovision ? ryz_provisioning_request_reprovision() :
                         ryz_provisioning_start()) == ESP_OK);
    ryz_provisioning_snapshot_t state = snapshot();
    assert(state.state == RYZ_PROVISIONING_ONLINE);
    assert(state.credentials_stored && !state.portal_active);
    assert(state.portal_ip[0] == '\0');
    assert(strcmp(state.sta_ssid, "NEW-NETWORK") == 0);
    assert(strcmp(state.ipv4, "10.1.2.3") == 0);
}

static ryz_provisioning_link_info_t link_sample(uint64_t at)
{
    return (ryz_provisioning_link_info_t){
        .mac_valid = true, .rssi_valid = true, .ipv6_valid = true,
        .mac = "02:12:34:56:78:9A", .ipv6 = "2001:db8::37", .rssi_dbm = 3,
        .sampled_at_us = at,
    };
}

static void assert_link_clear(void)
{
    ryz_provisioning_link_info_t link = snapshot().link;
    assert(!link.mac_valid && !link.rssi_valid && !link.ipv6_valid);
    assert(!link.mac[0] && !link.ipv6[0] && !link.rssi_dbm);
    assert(!link.sampled_at_us && link.last_error == ESP_OK);
}

static void test_link_is_bound_to_current_online_view(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("A", "eight888");
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    ryz_provisioning_link_info_t link = link_sample(1000000);
    uint32_t revision = snapshot().revision;
    fake_platform_link_info("A", "10.0.0.1", &link);
    assert(snapshot().revision == revision);
    assert_link_clear();
    fake_platform_got_ip("A", "10.0.0.1");
    fake_platform_link_info("A", "10.0.0.1", &link);
    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_ONLINE && value.last_error == ESP_OK);
    assert(value.link.mac_valid && value.link.rssi_valid && value.link.ipv6_valid);
    assert(value.link.rssi_dbm == 3 && !strcmp(value.link.mac, "02:12:34:56:78:9A"));
    revision = value.revision;
    link.sampled_at_us = 2000000;
    fake_platform_link_info("B", "10.0.0.1", &link);
    fake_platform_link_info("A", "10.0.0.2", &link);
    link.sampled_at_us = 999999;
    fake_platform_link_info("A", "10.0.0.1", &link);
    assert(snapshot().revision == revision && snapshot().link.sampled_at_us == 1000000);
    link = (ryz_provisioning_link_info_t){.sampled_at_us = 2000000, .last_error = ESP_FAIL};
    fake_platform_link_info("A", "10.0.0.1", &link);
    value = snapshot();
    assert(value.state == RYZ_PROVISIONING_ONLINE && value.last_error == ESP_OK);
    assert(!value.link.mac_valid && !value.link.rssi_valid && !value.link.ipv6_valid);
    assert(!value.link.mac[0] && !value.link.ipv6[0] && !value.link.rssi_dbm);
    assert(value.link.last_error == ESP_FAIL && value.link.sampled_at_us == 2000000);
    link = link_sample(3000000);
    fake_platform_link_info("A", "10.0.0.1", &link);
    assert(snapshot().link.rssi_valid && snapshot().link.last_error == ESP_OK);
}

static void link_during_stop(void)
{
    assert(snapshot().state == RYZ_PROVISIONING_STOPPING && !snapshot().enabled);
    assert_link_clear();
    ryz_provisioning_link_info_t link = link_sample(9000000);
    fake_platform_link_info("A", "10.0.0.1", &link);
    assert_link_clear();
}

static void test_link_is_revoked_on_every_connection_transition(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("A", "eight888");
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    ryz_provisioning_link_info_t link = link_sample(1000000);
    fake_platform_got_ip("A", "10.0.0.1");
    fake_platform_link_info("A", "10.0.0.1", &link);
    fake_platform_begin_connecting();
    assert_link_clear();
    fake_platform_link_info("A", "10.0.0.1", &link);
    assert_link_clear();
    fake_platform_got_ip("A", "10.0.0.1");
    fake_platform_link_info("A", "10.0.0.1", &link);
    fake_platform_submit_credentials("A", "eight888");
    assert_link_clear();
    fake_platform_got_ip("A", "10.0.0.1");
    fake_platform_link_info("A", "10.0.0.1", &link);
    fake_platform_disconnect(1);
    assert_link_clear();
    fake_platform_got_ip("A", "10.0.0.1");
    fake_platform_link_info("A", "10.0.0.1", &link);
    fake_platform_set_stop_hook(link_during_stop);
    fake_platform_set_stop_result(ESP_FAIL);
    assert(ryz_provisioning_set_enabled(false) == ESP_FAIL);
    assert_link_clear();
    assert(snapshot().state == RYZ_PROVISIONING_FAILED && !snapshot().enabled);
    fake_platform_set_stop_result(ESP_OK);
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    assert_link_clear();
    fake_platform_link_info("A", "10.0.0.1", &link);
    assert_link_clear();
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    fake_platform_link_info("A", "10.0.0.1", &link);
    assert(snapshot().state == RYZ_PROVISIONING_CONNECTING);
    assert_link_clear();
}

static void secret_stop_during_copy(void)
{ assert(ryz_provisioning_set_enabled(false) == ESP_OK); }

static void secret_buffer_empty(const char *out, size_t size)
{ for (size_t i=0; i<size; ++i) assert(out[i] == 0); }

static void test_secret_core_control_gate_and_postcheck(void)
{
    char out[64]; memset(out, 'x', sizeof(out));
    ryz_provisioning_local_secret_t meta = {.available=true, .epoch=99};
    assert(ryz_provisioning_local_secret_try_get(&meta) == ESP_ERR_INVALID_STATE);
    assert(!meta.available && !meta.epoch);
    assert(ryz_provisioning_local_secret_try_copy(7, out, sizeof(out)) == ESP_ERR_INVALID_STATE);
    secret_buffer_empty(out, sizeof(out));
    fake_platform_reset();
    fake_platform_store_credentials("A", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_start() == ESP_OK);
    assert(ryz_provisioning_local_secret_try_get(&meta) == ESP_OK && !meta.available);
    assert(fake_platform_local_secret_calls() == 0);
    fake_platform_got_ip("A", "10.0.0.1");
    assert(ryz_provisioning_local_secret_try_get(&meta) == ESP_OK && meta.epoch == 7);
    assert(ryz_provisioning_local_secret_try_copy(meta.epoch, out, sizeof(out)) == ESP_OK);
    assert(strcmp(out, "test-pass8") == 0);
    fake_platform_set_local_secret(ESP_ERR_TIMEOUT, NULL);
    assert(ryz_provisioning_local_secret_try_get(&meta) == ESP_ERR_TIMEOUT && !meta.epoch && !meta.available);
    assert(ryz_provisioning_local_secret_try_copy(7, out, sizeof(out)) == ESP_ERR_TIMEOUT);
    secret_buffer_empty(out, sizeof(out)); /* Adapter wrote then failed. */
    fake_platform_set_local_secret(ESP_OK, secret_stop_during_copy);
    assert(ryz_provisioning_local_secret_try_copy(7, out, sizeof(out)) == ESP_ERR_INVALID_STATE);
    secret_buffer_empty(out, sizeof(out));
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    fake_platform_got_ip("A", "10.0.0.1");
    assert(ryz_provisioning_local_secret_try_get(&meta) == ESP_OK && !meta.available && !meta.epoch);
    unsigned calls = fake_platform_local_secret_calls();
    assert(ryz_provisioning_local_secret_try_copy(7, out, sizeof(out)) == ESP_ERR_INVALID_STATE);
    assert(fake_platform_local_secret_calls() == calls);
    secret_buffer_empty(out, sizeof(out));
}

static void assert_secret_unavailable(void)
{
    ryz_provisioning_local_secret_t meta = {.available = true, .epoch = 9};
    assert(ryz_provisioning_local_secret_try_get(&meta) == ESP_OK);
    assert(!meta.available && !meta.epoch);
    char text[64]; memset(text, 'x', sizeof(text));
    assert(ryz_provisioning_local_secret_try_copy(7, text, sizeof(text)) == ESP_ERR_INVALID_STATE);
    secret_buffer_empty(text, sizeof(text));
}

static bool fail_load_after_stop;
static void open_portal_stop_hook(void)
{
    stop_hook();
    assert_secret_unavailable();
    assert_link_clear();
    /* Inject only once stop is executing: an implementation that reads before
     * confirming stop would miss this failure and incorrectly start AP. */
    if (fail_load_after_stop) fake_platform_set_load_result(ESP_FAIL);
}

static void open_portal_start_hook(void)
{
    assert(snapshot().state == RYZ_PROVISIONING_AP_STARTING && snapshot().enabled);
    assert(!snapshot().portal_active && !snapshot().ipv4[0]);
    assert_secret_unavailable();
    assert_link_clear();
    assert(ryz_provisioning_open_portal() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_request_reprovision() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_set_enabled(false) == ESP_ERR_INVALID_STATE);
}

static void test_open_portal_without_prior_start(bool stored)
{
    fake_platform_reset();
    assert(ryz_provisioning_open_portal() == ESP_ERR_INVALID_STATE);
    if (stored) fake_platform_store_credentials("SAVED", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK);
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    fake_platform_set_portal_start_hook(open_portal_start_hook);
    assert(ryz_provisioning_open_portal() == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY && snapshot().enabled);
    assert(snapshot().credentials_stored == stored);
    assert(!strcmp(snapshot().sta_ssid, stored ? "SAVED" : ""));
    assert(fake_platform_stop_count() == 0 && fake_platform_load_count() == 1);
    assert(fake_platform_start_portal_count() == 1 && fake_platform_clear_count() == 0);
    assert_secret_unavailable();
}

static void test_open_portal_failure(const char *mode)
{
    fake_platform_reset();
    fake_platform_store_credentials("KEEP", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    fake_platform_got_ip("KEEP", "10.0.0.1");
    const ryz_provisioning_link_info_t link = link_sample(1000000);
    fake_platform_link_info("KEEP", "10.0.0.1", &link);
    fake_platform_set_stop_hook(open_portal_stop_hook);
    fake_platform_set_portal_start_hook(open_portal_start_hook);
    const bool stopping = !strcmp(mode, "open-stop-failure");
    const bool reading = !strcmp(mode, "open-load-failure");
    if (stopping) fake_platform_set_stop_result(ESP_FAIL);
    else if (reading) fail_load_after_stop = true;
    else if (!strcmp(mode, "open-address-failure")) fake_platform_set_portal_ip("");
    else fake_platform_set_start_portal_result(ESP_FAIL);
    assert(ryz_provisioning_open_portal() == ESP_FAIL);
    ryz_provisioning_snapshot_t failed = snapshot();
    assert(failed.state == RYZ_PROVISIONING_FAILED && failed.last_error == ESP_FAIL);
    assert(failed.stop_confirmed == reading);
    assert(failed.enabled == (!stopping && !reading));
    assert(!failed.ipv4[0] && !failed.portal_ip[0] && !failed.portal_active);
    assert(failed.credentials_stored && !strcmp(failed.sta_ssid, "KEEP"));
    assert_link_clear(); assert_secret_unavailable();
    assert(fake_platform_credentials_exist() && fake_platform_clear_count() == 0);
    assert(fake_platform_start_portal_count() == (stopping || reading ? 0U : 1U));
    assert(fake_platform_load_count() == (stopping ? 1U : 2U));
    /* Explicit retry, not a timer or automatic restoration of STA. */
    fail_load_after_stop = false;
    fake_platform_set_stop_result(ESP_OK); fake_platform_set_load_result(ESP_OK);
    fake_platform_set_start_portal_result(ESP_OK); fake_platform_set_portal_ip("192.168.4.1");
    assert(ryz_provisioning_open_portal() == ESP_OK);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY && snapshot().credentials_stored);
    assert(!strcmp(snapshot().sta_ssid, "KEEP") && fake_platform_start_sta_count() == 1);
    assert(fake_platform_credentials_exist() && fake_platform_clear_count() == 0);
    /* Going back to saved STA proves its password was not erased or replaced. */
    assert(ryz_provisioning_set_enabled(false) == ESP_OK);
    assert(ryz_provisioning_set_enabled(true) == ESP_OK);
    fake_platform_got_ip("KEEP", "10.0.0.1");
    char password[64];
    assert(ryz_provisioning_local_secret_try_copy(7, password, sizeof(password)) == ESP_OK);
    assert(!strcmp(password, "test-pass8"));
}

static void test_open_portal_preserves_fast_submit(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("OLD", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    fake_platform_set_portal_start_hook(submit_before_start_returns);
    assert(ryz_provisioning_open_portal() == ESP_OK);
    ryz_provisioning_snapshot_t value = snapshot();
    assert(value.state == RYZ_PROVISIONING_ONLINE && value.credentials_stored);
    assert(!value.portal_active && !value.portal_ip[0]);
    assert(!strcmp(value.sta_ssid, "NEW-NETWORK") && !strcmp(value.ipv4, "10.1.2.3"));
    assert(fake_platform_clear_count() == 0);
}

static pthread_mutex_t open_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t open_cond = PTHREAD_COND_INITIALIZER;
static bool open_waiting, open_release;
static void held_open_stop(void)
{
    open_portal_stop_hook();
    pthread_mutex_lock(&open_mutex);
    open_waiting = true;
    pthread_cond_broadcast(&open_cond);
    while (!open_release) pthread_cond_wait(&open_cond, &open_mutex);
    pthread_mutex_unlock(&open_mutex);
}
static void *open_worker(void *unused)
{
    (void)unused;
    assert(ryz_provisioning_open_portal() == ESP_OK);
    return NULL;
}
static void test_open_portal_control_is_nonblocking(void)
{
    fake_platform_reset();
    fake_platform_store_credentials("KEEP", "test-pass8");
    assert(ryz_provisioning_init() == ESP_OK && ryz_provisioning_start() == ESP_OK);
    fake_platform_set_stop_hook(held_open_stop);
    pthread_t worker; assert(pthread_create(&worker, NULL, open_worker, NULL) == 0);
    pthread_mutex_lock(&open_mutex);
    while (!open_waiting) pthread_cond_wait(&open_cond, &open_mutex);
    pthread_mutex_unlock(&open_mutex);
    assert(snapshot().state == RYZ_PROVISIONING_STOPPING);
    assert(ryz_provisioning_open_portal() == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_set_enabled(true) == ESP_ERR_INVALID_STATE);
    assert(ryz_provisioning_request_reprovision() == ESP_ERR_INVALID_STATE);
    pthread_mutex_lock(&open_mutex); open_release = true; pthread_cond_broadcast(&open_cond);
    pthread_mutex_unlock(&open_mutex);
    assert(pthread_join(worker, NULL) == 0);
    assert(snapshot().state == RYZ_PROVISIONING_AP_READY && fake_platform_clear_count() == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "boot-initial-ap-fast-submit")) {
        test_initial_ap_submission_is_not_boot_restore();
    } else if (!strncmp(argv[1], "boot-fallback-", 14)) {
        test_boot_fallback((unsigned)atoi(argv[1] + 14));
    } else if (!strcmp(argv[1], "pref-boot-off") || !strcmp(argv[1], "pref-boot-on")) {
        test_preference_boot(!strcmp(argv[1], "pref-boot-on"));
    } else if (!strcmp(argv[1], "pref-init-retry")) {
        test_preference_init_retry();
    } else if (!strcmp(argv[1], "pref-missing")) {
        test_missing_preference_first_explicit_on();
    } else if (!strcmp(argv[1], "pref-save-failure") || !strcmp(argv[1], "pref-ambiguous")) {
        test_preference_save_failure(!strcmp(argv[1], "pref-ambiguous"));
    } else if (!strcmp(argv[1], "pref-runtime-failure")) {
        test_preference_runtime_failure_does_not_rollback();
    } else if (!strcmp(argv[1], "pref-temporary-ap") || !strcmp(argv[1], "pref-temporary-forget")) {
        test_temporary_ap_does_not_save_preference(!strcmp(argv[1], "pref-temporary-forget"));
    } else if (strcmp(argv[1], "open-portal") == 0) {
        test_open_portal_keeps_saved_network();
    } else if (!strcmp(argv[1], "open-without-start")) {
        test_open_portal_without_prior_start(false);
    } else if (!strcmp(argv[1], "open-off-stored")) {
        test_open_portal_without_prior_start(true);
    } else if (!strcmp(argv[1], "open-stop-failure") || !strcmp(argv[1], "open-load-failure") ||
               !strcmp(argv[1], "open-start-failure") || !strcmp(argv[1], "open-address-failure")) {
        test_open_portal_failure(argv[1]);
    } else if (!strcmp(argv[1], "open-fast-submit")) {
        test_open_portal_preserves_fast_submit();
    } else if (!strcmp(argv[1], "open-concurrent")) {
        test_open_portal_control_is_nonblocking();
    } else if (strcmp(argv[1], "local-secret") == 0) {
        test_secret_core_control_gate_and_postcheck();
    } else if (strcmp(argv[1], "link-binding") == 0) {
        test_link_is_bound_to_current_online_view();
    } else if (strcmp(argv[1], "link-revocation") == 0) {
        test_link_is_revoked_on_every_connection_transition();
    } else if (strcmp(argv[1], "portal-publish-race") == 0) {
        test_portal_start_cannot_overwrite_newer_event(false);
    } else if (strcmp(argv[1], "reprovision-publish-race") == 0) {
        test_portal_start_cannot_overwrite_newer_event(true);
    } else if (strcmp(argv[1], "power-cycle") == 0) {
        test_power_cycle_keeps_credentials();
    } else if (strcmp(argv[1], "stop-confirmation") == 0) {
        test_stop_failure_requires_confirmation();
    } else if (strcmp(argv[1], "forget-failure") == 0) {
        test_forget_failure_preserves_saved_identity();
    } else if (strcmp(argv[1], "off-before-start") == 0) {
        test_disable_before_first_start();
    } else if (strcmp(argv[1], "portal") == 0) {
        test_unconfigured_portal_to_online();
    } else if (strcmp(argv[1], "stored") == 0) {
        test_stored_credentials_start_in_station_mode();
    } else if (strcmp(argv[1], "reprovision") == 0) {
        test_reprovision_clears_credentials_and_reopens_portal();
    } else if (strcmp(argv[1], "failure") == 0) {
        test_start_failures_are_observable();
    } else if (strcmp(argv[1], "portal-address-failure") == 0) {
        test_portal_without_an_ipv4_address_is_not_ready();
    } else if (strcmp(argv[1], "portal-runtime-failure") == 0) {
        test_platform_failure_clears_active_portal_address();
    } else if (strcmp(argv[1], "threadsafe") == 0) {
        test_snapshot_is_coherent_during_event_updates();
    } else if (strcmp(argv[1], "generation") == 0) {
        test_stale_connect_command_does_not_cancel_new_pending_command();
    } else if (strcmp(argv[1], "reconnect") == 0) {
        test_reconnect_policy_is_bounded_and_backoff_increases();
        test_auth_failure_and_mode_switch_cancel_reconnects();
        test_online_keeps_station_epoch_for_following_disconnect();
        test_pending_online_observation_wins_reconnect_deadline();
    } else if (strcmp(argv[1], "init-plan") == 0) {
        test_partial_initialization_rolls_back_only_owned_resources();
    } else if (strcmp(argv[1], "init-retry") == 0) {
        test_identity_failure_rolls_back_platform_and_init_can_retry();
    } else if (strcmp(argv[1], "idempotent-start") == 0) {
        test_start_is_idempotent_while_station_is_active();
    } else {
        return 2;
    }
    puts("RYZ_PROVISIONING_STATE_PASS");
    return 0;
}
