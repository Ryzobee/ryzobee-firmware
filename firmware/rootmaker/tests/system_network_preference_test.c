#define _POSIX_C_SOURCE 200809L

/* Reuse the existing controlled pthread owner, not its provisioning substitute.
 * The real service owner and provisioning core are separate translation units. */
#include "ryz_provisioning.h"
#define ryz_provisioning_init fixture_provisioning_init
#define ryz_provisioning_start fixture_provisioning_start
#define ryz_provisioning_get_snapshot fixture_provisioning_get_snapshot
#define ryz_provisioning_set_enabled fixture_provisioning_set_enabled
#define ryz_provisioning_open_portal fixture_provisioning_open_portal
#define ryz_provisioning_boot_fallback fixture_provisioning_boot_fallback
#define ryz_provisioning_request_reprovision fixture_provisioning_request_reprovision
#define main system_services_fixture_main
#include "system_services_test.c"
#undef main
#undef ryz_provisioning_request_reprovision
#undef ryz_provisioning_boot_fallback
#undef ryz_provisioning_open_portal
#undef ryz_provisioning_set_enabled
#undef ryz_provisioning_get_snapshot
#undef ryz_provisioning_start
#undef ryz_provisioning_init

#include "ryz_provisioning_platform.h"

/* Storage and radio are external Adapters. This does not exercise real NVS,
 * Wi-Fi, SNTP, OTA handles, a display, or the image-confirmation SDK call. */
static pthread_mutex_t provisioning_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned provisioning_lock_depth;
static ryz_provisioning_platform_event_sink_t provisioning_sink;
static bool preference_present = true, preference_enabled;
static esp_err_t preference_load_error, preference_save_error;
static unsigned preference_loads, preference_saves;
static unsigned provisioning_inits, provisioning_deinits;
static unsigned credential_loads, credential_clears, radio_sta_starts, radio_ap_starts, radio_stops;
static unsigned boot_fallback_calls;
static unsigned saved_scans;
static esp_err_t saved_scan_error;

void ryz_provisioning_platform_snapshot_lock(void)
{
    assert(!provisioning_lock_depth && !s_lock_depth);
    assert(pthread_mutex_lock(&provisioning_mutex) == 0);
    ++provisioning_lock_depth;
}

void ryz_provisioning_platform_snapshot_unlock(void)
{
    assert(provisioning_lock_depth == 1);
    --provisioning_lock_depth;
    assert(pthread_mutex_unlock(&provisioning_mutex) == 0);
}

static void assert_provisioning_owner(void)
{
    assert(pthread_equal(pthread_self(), s_worker));
    assert(!s_lock_depth && !provisioning_lock_depth);
    ryz_system_services_snapshot_t value;
    assert(ryz_system_services_get_snapshot(&value) == ESP_OK);
}

esp_err_t ryz_provisioning_platform_init(ryz_provisioning_platform_event_sink_t sink)
{
    assert_provisioning_owner();
    assert(sink && !provisioning_sink);
    provisioning_sink = sink;
    ++provisioning_inits;
    return ESP_OK;
}

void ryz_provisioning_platform_deinit(void)
{
    assert_provisioning_owner();
    provisioning_sink = NULL;
    ++provisioning_deinits;
}

esp_err_t ryz_provisioning_platform_load_enabled(bool *out)
{
    assert_provisioning_owner();
    assert(out);
    ++preference_loads;
    *out = false;
    if (preference_load_error != ESP_OK) return preference_load_error;
    if (!preference_present) return ESP_ERR_NOT_FOUND;
    *out = preference_enabled;
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_save_enabled(bool enabled)
{
    assert_provisioning_owner();
    /* Admission must hold OTA and revoke online prerequisites before even the
     * persistence stage, not only before the subsequent radio operation. */
    assert(atomic_load(&s_ota_held));
    assert(!s_ota.worker_active && s_ota.cleanup_confirmed);
    assert(s_prerequisite_count && !s_prerequisites[s_prerequisite_count - 1].network);
    ++preference_saves;
    if (preference_save_error != ESP_OK) return preference_save_error;
    preference_present = true;
    preference_enabled = enabled;
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_get_ap_identity(char *ssid, size_t ssid_size,
                                                    char *password, size_t password_size)
{
    assert_provisioning_owner();
    assert(ssid_size > strlen("fixture-ap") && password_size > strlen("fixture-only"));
    strcpy(ssid, "fixture-ap");
    strcpy(password, "fixture-only");
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_load_credentials(ryz_provisioning_credentials_t *out)
{
    assert_provisioning_owner();
    ++credential_loads;
    memset(out, 0, sizeof(*out));
    strcpy(out->ssid, "fixture-sta");
    strcpy(out->password, "fixture-only");
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_clear_credentials(void)
{
    assert_provisioning_owner();
    ++credential_clears;
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_start_sta(const ryz_provisioning_credentials_t *credentials)
{
    assert_provisioning_owner();
    assert(credentials && !strcmp(credentials->ssid, "fixture-sta"));
    ++radio_sta_starts;
    ryz_provisioning_platform_event_t event = {.type = RYZ_PROVISIONING_PLATFORM_ONLINE};
    strcpy(event.ssid, credentials->ssid);
    strcpy(event.ipv4, "192.0.2.3");
    provisioning_sink(&event); /* Platform fact; the real core accepts/publishes it. */
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_start_portal(ryz_provisioning_portal_info_t *out)
{
    assert_provisioning_owner();
    ++radio_ap_starts;
    memset(out, 0, sizeof(*out));
    strcpy(out->ipv4, "192.0.2.1");
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_stop(void)
{
    assert_provisioning_owner();
    ++radio_stops;
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_start_saved_sta(const ryz_provisioning_credentials_t *credentials)
{
    assert_provisioning_owner();
    ryz_provisioning_snapshot_t view;
    assert(ryz_provisioning_get_snapshot(&view) == ESP_OK);
    assert(view.boot_phase == RYZ_PROVISIONING_BOOT_SCANNING);
    ++saved_scans;
    if (saved_scan_error != ESP_OK) return saved_scan_error;
    const ryz_provisioning_platform_event_t event = {.type = RYZ_PROVISIONING_PLATFORM_CONNECTING};
    provisioning_sink(&event);
    return ryz_provisioning_platform_start_sta(credentials);
}

esp_err_t ryz_provisioning_platform_boot_fallback(void)
{
    assert_provisioning_owner();
    ++boot_fallback_calls;
    if (saved_scan_error != ESP_OK) {
        ryz_provisioning_platform_event_t event = {.type=RYZ_PROVISIONING_PLATFORM_BOOT_AP_STARTING};
        provisioning_sink(&event);
        ryz_provisioning_portal_info_t portal;
        assert(ryz_provisioning_platform_start_portal(&portal)==ESP_OK);
        event.type=RYZ_PROVISIONING_PLATFORM_PORTAL_READY;
        strcpy(event.ipv4,portal.ipv4);
        provisioning_sink(&event);
        return ESP_OK;
    }
    /* These cases restore OFF, fail before startup, or synchronously report a
     * genuine platform ONLINE observation. None may request fallback radio I/O. */
    assert(!"unexpected boot fallback in preference integration");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ryz_provisioning_platform_local_secret_try_get(ryz_provisioning_local_secret_t *out)
{
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ryz_provisioning_platform_local_secret_try_copy(uint64_t epoch, char *out, size_t size)
{
    (void)epoch;
    if (out && size) memset(out, 0, size);
    return ESP_ERR_NOT_SUPPORTED;
}

static void start_owner(void)
{
    s_main = pthread_self();
    assert(ryz_system_services_start() == ESP_OK);
    cycle(0);
}

static void saved_off_case(void)
{
    preference_enabled = false;
    start_owner();
    const ryz_system_services_snapshot_t first = snapshot();
    assert(first.cycles == 1 && first.boot_network_settled);
    assert(first.boot_network_error == ESP_OK);
    assert(!credential_loads && !radio_sta_starts && !radio_ap_starts && !radio_stops);
    assert(!boot_fallback_calls); /* OFF completes at time zero, not after 10 s. */
    cycle(INT64_C(200000));
    cycle(INT64_C(5000000));
    cycle(INT64_C(10000000));
    ryz_system_services_snapshot_t value = snapshot();
    assert(value.provisioning_ready && value.provisioning_started && value.network_valid);
    assert(value.boot_network_settled && value.boot_network_error == ESP_OK);
    assert(value.time_ready && value.ota_ready);
    assert(value.network.state == RYZ_PROVISIONING_OFF && !value.network.enabled);
    assert(value.network.stop_confirmed && !value.network.portal_active && !value.network.ipv4[0]);
    assert(value.errors[RYZ_SYSTEM_PROVISIONING_START] == ESP_OK);
    assert(!value.network_operation.requested && !value.network_operation.completed);
    assert(provisioning_inits == 1 && preference_loads == 1 && !provisioning_deinits);
    assert(!preference_saves && !credential_loads && !credential_clears);
    assert(!radio_sta_starts && !radio_ap_starts && !radio_stops && !boot_fallback_calls);
    assert(s_time_network_count > 0 && s_prerequisite_count > 0);
    for (unsigned i = 0; i < s_time_network_count; ++i) assert(!s_time_network_arguments[i]);
    for (unsigned i = 0; i < s_prerequisite_count; ++i) assert(!s_prerequisites[i].network);
    assert_prerequisites(false, false);
}

static void assert_same_radio(const ryz_provisioning_snapshot_t *before,
                               const ryz_provisioning_snapshot_t *after)
{
    assert(after->revision == before->revision && after->state == before->state);
    assert(after->enabled == before->enabled && after->stop_confirmed == before->stop_confirmed);
    assert(after->portal_active == before->portal_active && after->last_error == before->last_error);
    assert(!strcmp(after->ipv4, before->ipv4) && !strcmp(after->sta_ssid, before->sta_ssid));
}

static void save_failure_case(bool target_enabled)
{
    preference_enabled = !target_enabled;
    start_owner();
    const ryz_provisioning_snapshot_t before = snapshot().network;
    const unsigned previous_starts = radio_sta_starts, previous_stops = radio_stops;
    const esp_err_t failure = target_enabled ? ESP_ERR_NO_MEM : ESP_FAIL;
    const ryz_system_network_action_t action = target_enabled ? RYZ_SYSTEM_NETWORK_ON : RYZ_SYSTEM_NETWORK_OFF;
    preference_save_error = failure;
    uint32_t first = 0;
    assert(ryz_system_services_request_network(action, &first) == ESP_OK && first != 0);
    assert(atomic_load(&s_ota_held) && preference_saves == 0);
    cycle(INT64_C(200000));
    ryz_system_services_snapshot_t failed = snapshot();
    assert(failed.network_operation.requested == first && failed.network_operation.completed == first);
    assert(failed.network_operation.completed_action == action);
    assert(failed.network_operation.phase == RYZ_SYSTEM_NETWORK_FAILED);
    assert(failed.network_operation.result == failure);
    assert(failed.network_operation.completed_network_valid);
    assert(failed.network_operation.completed_network_state == before.state);
    assert(failed.errors[RYZ_SYSTEM_NETWORK_CONTROL] == failure);
    assert_same_radio(&before, &failed.network);
    ryz_provisioning_snapshot_t actual;
    assert(ryz_provisioning_get_snapshot(&actual) == ESP_OK);
    assert_same_radio(&before, &actual);
    assert(preference_saves == 1 && preference_enabled == !target_enabled);
    assert(radio_sta_starts == previous_starts && radio_stops == previous_stops && !radio_ap_starts);
    assert(!atomic_load(&s_ota_held));
    assert_prerequisites(!target_enabled, false); /* Restore the actual unchanged link. */
    cycle(INT64_C(5000000));
    cycle(INT64_C(10000000));
    assert(preference_saves == 1 && radio_sta_starts == previous_starts && radio_stops == previous_stops);
    assert(snapshot().network_operation.completed == first); /* No implicit retry. */

    preference_save_error = ESP_OK;
    uint32_t retry = 0;
    assert(ryz_system_services_request_network(action, &retry) == ESP_OK && retry > first);
    assert(atomic_load(&s_ota_held) && preference_saves == 1);
    cycle(INT64_C(10200000));
    ryz_system_services_snapshot_t done = snapshot();
    assert(done.network_operation.completed == retry && done.network_operation.result == ESP_OK);
    assert(done.network_operation.phase == RYZ_SYSTEM_NETWORK_DONE);
    assert(done.network_operation.completed_action == action && done.network_operation.completed_network_valid);
    assert(done.network.enabled == target_enabled && done.network.stop_confirmed == !target_enabled);
    assert(done.network.state == (target_enabled ? RYZ_PROVISIONING_ONLINE : RYZ_PROVISIONING_OFF));
    assert(preference_saves == 2 && preference_enabled == target_enabled);
    assert(radio_sta_starts == previous_starts + (target_enabled ? 1U : 0U));
    assert(radio_stops == previous_stops + (target_enabled ? 0U : 1U));
    assert(!radio_ap_starts && !credential_clears && !atomic_load(&s_ota_held));
    assert_prerequisites(target_enabled, false);
}

static void load_failure_case(bool recovered_enabled)
{
    preference_load_error = ESP_ERR_INVALID_RESPONSE;
    start_owner();
    ryz_system_services_snapshot_t failed = snapshot();
    assert(!failed.provisioning_ready && !failed.provisioning_started && !failed.network_valid);
    assert(failed.boot_network_settled && failed.boot_network_error == ESP_ERR_INVALID_RESPONSE);
    assert(!failed.boot_network_deadline_us);
    assert(failed.errors[RYZ_SYSTEM_PROVISIONING_INIT] == ESP_ERR_INVALID_RESPONSE);
    assert(provisioning_inits == 1 && preference_loads == 1 && provisioning_deinits == 1);
    assert(!radio_sta_starts && !radio_ap_starts && !radio_stops && !preference_saves);
    assert_prerequisites(false, false);
    uint32_t rejected = 123;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_ON, &rejected) == ESP_ERR_INVALID_STATE);
    assert(!rejected && !atomic_load(&s_ota_held));
    cycle(INT64_C(4999999));
    assert(provisioning_inits == 1 && preference_loads == 1);
    assert(snapshot().boot_network_settled && !credential_loads && !boot_fallback_calls);
    preference_load_error = ESP_OK;
    preference_enabled = recovered_enabled;
    cycle(INT64_C(5000000));
    ryz_system_services_snapshot_t recovered = snapshot();
    assert(recovered.provisioning_ready && !recovered.provisioning_started && recovered.network_valid);
    assert(recovered.network.state == (recovered_enabled ? RYZ_PROVISIONING_UNCONFIGURED : RYZ_PROVISIONING_OFF));
    assert(recovered.network.enabled == recovered_enabled && recovered.network.stop_confirmed);
    assert(recovered.boot_network_settled && recovered.boot_network_error == ESP_ERR_INVALID_RESPONSE);
    assert(!recovered.boot_network_deadline_us);
    assert(recovered.errors[RYZ_SYSTEM_PROVISIONING_INIT] == ESP_OK);
    assert(provisioning_inits == 2 && preference_loads == 2 && provisioning_deinits == 1);
    assert(!radio_sta_starts && !radio_ap_starts && !radio_stops && !preference_saves);
    assert(!credential_loads && !credential_clears && !boot_fallback_calls);
    assert_prerequisites(false, false);
    cycle(INT64_C(10000000));
    cycle(INT64_C(20000000));
    assert(!snapshot().provisioning_started && !credential_loads && !radio_sta_starts);
    assert(!radio_ap_starts && !radio_stops && !boot_fallback_calls);

    /* Initialization retry may recover storage, but only a new explicit user
     * choice may activate the network after the failed boot decision settled. */
    uint32_t operation = 0;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_ON, &operation) == ESP_OK);
    assert(operation && atomic_load(&s_ota_held));
    cycle(INT64_C(20200000));
    const ryz_system_services_snapshot_t online = snapshot();
    assert(online.network_operation.completed == operation && online.network_operation.result == ESP_OK);
    assert(online.network_operation.phase == RYZ_SYSTEM_NETWORK_DONE);
    assert(online.provisioning_started && online.network.state == RYZ_PROVISIONING_ONLINE);
    assert(online.network.enabled && !online.network.stop_confirmed && online.network.ipv4[0]);
    assert(online.boot_network_settled && online.boot_network_error == ESP_ERR_INVALID_RESPONSE);
    assert(credential_loads == 1 && radio_sta_starts == 1);
    assert(preference_saves == (recovered_enabled ? 0U : 1U));
    assert(!radio_ap_starts && !radio_stops && !credential_clears && !boot_fallback_calls);
    assert(!atomic_load(&s_ota_held));
    assert_prerequisites(true, false);
}

static void ota_wait_case(void)
{
    preference_enabled = true;
    start_owner();
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE && radio_sta_starts == 1);
    s_ota.worker_active = true;
    s_ota.cleanup_confirmed = false;
    uint32_t operation = 0;
    assert(ryz_system_services_request_network(RYZ_SYSTEM_NETWORK_OFF, &operation) == ESP_OK);
    cycle(INT64_C(200000));
    cycle(INT64_C(5000000));
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_WAITING_OTA);
    assert(!snapshot().network_operation.completed && atomic_load(&s_ota_held));
    assert(preference_enabled && !preference_saves && !radio_stops);
    assert(snapshot().network.state == RYZ_PROVISIONING_ONLINE);
    assert_prerequisites(false, false);
    s_ota.worker_active = false;
    s_ota.cleanup_confirmed = true;
    cycle(INT64_C(5200000));
    assert(snapshot().network_operation.completed == operation);
    assert(snapshot().network_operation.phase == RYZ_SYSTEM_NETWORK_DONE);
    assert(snapshot().network.state == RYZ_PROVISIONING_OFF);
    assert(preference_saves == 1 && !preference_enabled && radio_stops == 1);
    assert(!atomic_load(&s_ota_held));
    assert_prerequisites(false, false);
}

static void finish_owner(void)
{
    assert(pthread_mutex_lock(&s_gate) == 0);
    s_stopping = true;
    assert(pthread_cond_broadcast(&s_changed) == 0);
    assert(pthread_mutex_unlock(&s_gate) == 0);
    assert(pthread_join(s_worker, NULL) == 0);
    assert(pthread_mutex_destroy(&provisioning_mutex) == 0);
}

static void saved_scan_failure_case(bool missing)
{
    preference_enabled=true;
    saved_scan_error=missing ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
    start_owner();
    const ryz_system_services_snapshot_t view=snapshot();
    assert(view.cycles==1 && view.boot_network_settled); /* No ten-second wait. */
    assert(view.boot_network_error==ESP_OK); /* AP settlement, not STA success. */
    assert(view.network.state==RYZ_PROVISIONING_AP_READY && view.network.portal_active);
    assert(view.network.boot_phase==(missing ? RYZ_PROVISIONING_BOOT_NOT_FOUND : RYZ_PROVISIONING_BOOT_SCAN_FAILED));
    assert(saved_scans==1 && !radio_sta_starts && radio_ap_starts==1 && boot_fallback_calls==1);
    cycle(INT64_C(20000000));
    assert(saved_scans==1 && !radio_sta_starts && radio_ap_starts==1);
    assert(!credential_clears && !preference_saves && preference_enabled);
    assert_prerequisites(false,false);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "saved_off")) saved_off_case();
    else if (!strcmp(argv[1], "on_save_failure")) save_failure_case(true);
    else if (!strcmp(argv[1], "off_save_failure")) save_failure_case(false);
    else if (!strcmp(argv[1], "load_failure")) load_failure_case(false);
    else if (!strcmp(argv[1], "load_failure_on")) load_failure_case(true);
    else if (!strcmp(argv[1], "ota_wait")) ota_wait_case();
    else if (!strcmp(argv[1], "scan_missing")) saved_scan_failure_case(true);
    else if (!strcmp(argv[1], "scan_failed")) saved_scan_failure_case(false);
    else abort();
    finish_owner();
    printf("SYSTEM_NETWORK_PREFERENCE_PASS %s\n", argv[1]);
    return 0;
}
