#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "fake_platform.h"
#include "ryz_provisioning_platform.h"

static pthread_mutex_t s_snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static ryz_provisioning_platform_event_sink_t s_sink;
static ryz_provisioning_credentials_t s_credentials;
static bool s_has_credentials;
static esp_err_t s_start_portal_result;
static esp_err_t s_load_result;
static unsigned int s_load_count, s_clear_count, s_start_portal_count;
static esp_err_t s_start_sta_result;
static esp_err_t s_ap_identity_result;
static char s_portal_ip[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
static bool s_portal_start_observed_clean_snapshot;
static unsigned int s_init_count;
static unsigned int s_deinit_count;
static unsigned int s_start_sta_count;
static unsigned int s_stop_count;
static esp_err_t s_stop_result, s_clear_result;
static void (*s_stop_hook)(void);
static void (*s_portal_start_hook)(void);
static esp_err_t s_secret_result;
static void (*s_secret_hook)(void);
static unsigned int s_secret_calls;
static esp_err_t s_enabled_load_result, s_enabled_save_result;
static bool s_saved_enabled, s_enabled_writes_on_failure;
static unsigned int s_enabled_load_count, s_enabled_save_count;
static void (*s_enabled_save_hook)(void);
static esp_err_t s_saved_scan_result;
static unsigned s_saved_scan_count;

void fake_platform_reset(void)
{
    s_sink = NULL;
    memset(&s_credentials, 0, sizeof(s_credentials));
    s_has_credentials = false;
    s_start_portal_result = ESP_OK;
    s_load_result = ESP_OK;
    s_load_count = s_clear_count = s_start_portal_count = 0;
    s_start_sta_result = ESP_OK;
    s_ap_identity_result = ESP_OK;
    snprintf(s_portal_ip, sizeof(s_portal_ip), "192.168.4.1");
    s_portal_start_observed_clean_snapshot = false;
    s_init_count = 0;
    s_deinit_count = 0;
    s_start_sta_count = 0;
    s_stop_count = 0;
    s_stop_result = s_clear_result = ESP_OK;
    s_stop_hook = NULL;
    s_portal_start_hook = NULL;
    s_secret_result = ESP_OK;
    s_secret_hook = NULL;
    s_secret_calls = 0;
    s_enabled_load_result = ESP_ERR_NOT_FOUND;
    s_enabled_save_result = ESP_OK;
    s_saved_enabled = s_enabled_writes_on_failure = false;
    s_enabled_load_count = s_enabled_save_count = 0;
    s_enabled_save_hook = NULL;
    s_saved_scan_result = ESP_OK;
    s_saved_scan_count = 0;
}

void fake_platform_set_saved_scan_result(esp_err_t error) { s_saved_scan_result = error; }
unsigned fake_platform_saved_scan_count(void) { return s_saved_scan_count; }

void fake_platform_set_enabled_record(esp_err_t result, bool enabled)
{ s_enabled_load_result = result; s_saved_enabled = enabled; }
void fake_platform_set_save_enabled_result(esp_err_t result, bool writes_on_failure)
{ s_enabled_save_result = result; s_enabled_writes_on_failure = writes_on_failure; }
void fake_platform_set_save_enabled_hook(void (*hook)(void)) { s_enabled_save_hook = hook; }
unsigned int fake_platform_enabled_load_count(void) { return s_enabled_load_count; }
unsigned int fake_platform_enabled_save_count(void) { return s_enabled_save_count; }
bool fake_platform_saved_enabled(void) { return s_saved_enabled; }
esp_err_t ryz_provisioning_platform_load_enabled(bool *out)
{
    ++s_enabled_load_count;
    *out = s_enabled_load_result == ESP_OK && s_saved_enabled;
    return s_enabled_load_result;
}
esp_err_t ryz_provisioning_platform_save_enabled(bool enabled)
{
    ++s_enabled_save_count;
    if (s_enabled_save_hook) s_enabled_save_hook();
    if (s_enabled_save_result == ESP_OK || s_enabled_writes_on_failure) {
        s_saved_enabled = enabled;
        s_enabled_load_result = ESP_OK;
    }
    return s_enabled_save_result;
}

void fake_platform_set_local_secret(esp_err_t result, void (*hook)(void))
{ s_secret_result = result; s_secret_hook = hook; }
unsigned int fake_platform_local_secret_calls(void) { return s_secret_calls; }
esp_err_t ryz_provisioning_platform_local_secret_try_get(ryz_provisioning_local_secret_t *out)
{
    ++s_secret_calls;
    *out = (ryz_provisioning_local_secret_t){.available=true, .epoch=7};
    if (s_secret_hook) s_secret_hook();
    return s_secret_result;
}
esp_err_t ryz_provisioning_platform_local_secret_try_copy(uint64_t expected, char *out, size_t cap)
{
    ++s_secret_calls;
    if (expected != 7) return ESP_ERR_INVALID_STATE;
    if (cap <= strlen(s_credentials.password)) return ESP_ERR_INVALID_SIZE;
    strcpy(out, s_credentials.password);
    if (s_secret_hook) s_secret_hook();
    return s_secret_result;
}

void fake_platform_set_stop_result(esp_err_t result) { s_stop_result = result; }
void fake_platform_set_clear_result(esp_err_t result) { s_clear_result = result; }
void fake_platform_set_stop_hook(void (*hook)(void)) { s_stop_hook = hook; }
void fake_platform_set_portal_start_hook(void (*hook)(void)) { s_portal_start_hook = hook; }
unsigned int fake_platform_stop_count(void) { return s_stop_count; }

esp_err_t ryz_provisioning_platform_stop(void)
{
    ++s_stop_count;
    if (s_stop_hook) s_stop_hook();
    return s_stop_result;
}

void fake_platform_store_credentials(const char *ssid, const char *password)
{
    snprintf(s_credentials.ssid, sizeof(s_credentials.ssid), "%s", ssid);
    snprintf(s_credentials.password, sizeof(s_credentials.password), "%s", password);
    s_has_credentials = true;
}

void fake_platform_set_portal_ip(const char *ipv4)
{
    snprintf(s_portal_ip, sizeof(s_portal_ip), "%s", ipv4);
}

void fake_platform_set_start_portal_result(esp_err_t result)
{
    s_start_portal_result = result;
}

void fake_platform_set_load_result(esp_err_t result) { s_load_result = result; }
unsigned int fake_platform_load_count(void) { return s_load_count; }
unsigned int fake_platform_clear_count(void) { return s_clear_count; }
unsigned int fake_platform_start_portal_count(void) { return s_start_portal_count; }

void fake_platform_set_start_sta_result(esp_err_t result)
{
    s_start_sta_result = result;
}

void fake_platform_set_ap_identity_result(esp_err_t result)
{
    s_ap_identity_result = result;
}

static void emit(ryz_provisioning_platform_event_t event)
{
    if (s_sink != NULL) {
        s_sink(&event);
    }
}

void fake_platform_submit_credentials(const char *ssid, const char *password)
{
    fake_platform_store_credentials(ssid, password);
    ryz_provisioning_platform_event_t event = {
        .type = RYZ_PROVISIONING_PLATFORM_CREDENTIALS_RECEIVED,
    };
    snprintf(event.ssid, sizeof(event.ssid), "%s", ssid);
    emit(event);
}

void fake_platform_begin_connecting(void)
{
    ryz_provisioning_platform_event_t event = {
        .type = RYZ_PROVISIONING_PLATFORM_CONNECTING,
    };
    emit(event);
}

void fake_platform_got_ip(const char *ssid, const char *ipv4)
{
    ryz_provisioning_platform_event_t event = {
        .type = RYZ_PROVISIONING_PLATFORM_ONLINE,
    };
    snprintf(event.ssid, sizeof(event.ssid), "%s", ssid);
    snprintf(event.ipv4, sizeof(event.ipv4), "%s", ipv4);
    emit(event);
}

void fake_platform_disconnect(int reason)
{
    ryz_provisioning_platform_event_t event = {
        .type = RYZ_PROVISIONING_PLATFORM_FAILED,
        .error = ESP_FAIL,
        .detail = reason,
    };
    emit(event);
}

void fake_platform_link_info(const char *ssid, const char *ipv4,
                              const ryz_provisioning_link_info_t *link)
{
    ryz_provisioning_platform_event_t event = {
        .type = RYZ_PROVISIONING_PLATFORM_LINK_INFO, .link = *link,
    };
    snprintf(event.ssid, sizeof(event.ssid), "%s", ssid);
    snprintf(event.ipv4, sizeof(event.ipv4), "%s", ipv4);
    emit(event);
}

bool fake_platform_credentials_exist(void)
{
    return s_has_credentials;
}

bool fake_platform_portal_start_observed_clean_snapshot(void)
{
    return s_portal_start_observed_clean_snapshot;
}

unsigned int fake_platform_init_count(void)
{
    return s_init_count;
}

unsigned int fake_platform_deinit_count(void)
{
    return s_deinit_count;
}

unsigned int fake_platform_start_sta_count(void)
{
    return s_start_sta_count;
}

void ryz_provisioning_platform_snapshot_lock(void)
{
    pthread_mutex_lock(&s_snapshot_mutex);
}

void ryz_provisioning_platform_snapshot_unlock(void)
{
    pthread_mutex_unlock(&s_snapshot_mutex);
}

esp_err_t ryz_provisioning_platform_init(ryz_provisioning_platform_event_sink_t sink)
{
    ++s_init_count;
    if (s_sink != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sink = sink;
    return ESP_OK;
}

void ryz_provisioning_platform_deinit(void)
{
    ++s_deinit_count;
    s_sink = NULL;
}

esp_err_t ryz_provisioning_platform_get_ap_identity(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size)
{
    if (s_ap_identity_result != ESP_OK) {
        return s_ap_identity_result;
    }
    snprintf(ssid, ssid_size, "RYZOBEE-12AB");
    snprintf(password, password_size, "M7Q9X4K2P8TZ");
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_load_credentials(
    ryz_provisioning_credentials_t *credentials)
{
    ++s_load_count;
    if (s_load_result != ESP_OK) return s_load_result;
    if (!s_has_credentials) {
        return ESP_ERR_NOT_FOUND;
    }
    *credentials = s_credentials;
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_start_portal(
    ryz_provisioning_portal_info_t *out_portal_info)
{
    ++s_start_portal_count;
    if (out_portal_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ryz_provisioning_snapshot_t current = {0};
    s_portal_start_observed_clean_snapshot =
        ryz_provisioning_get_snapshot(&current) == ESP_OK &&
        current.state == RYZ_PROVISIONING_AP_STARTING &&
        !current.portal_active && current.portal_ip[0] == '\0';
    memset(out_portal_info, 0, sizeof(*out_portal_info));
    if (s_start_portal_result == ESP_OK) {
        snprintf(
            out_portal_info->ipv4,
            sizeof(out_portal_info->ipv4),
            "%s",
            s_portal_ip);
    }
    if (s_portal_start_hook) s_portal_start_hook();
    return s_start_portal_result;
}

esp_err_t ryz_provisioning_platform_start_sta(
    const ryz_provisioning_credentials_t *credentials)
{
    (void)credentials;
    ++s_start_sta_count;
    return s_start_sta_result;
}

esp_err_t ryz_provisioning_platform_boot_fallback(void)
{
    emit((ryz_provisioning_platform_event_t){.type = RYZ_PROVISIONING_PLATFORM_BOOT_AP_STARTING});
    esp_err_t error = ryz_provisioning_platform_stop();
    ryz_provisioning_portal_info_t portal = {0};
    if (error == ESP_OK) error = ryz_provisioning_platform_start_portal(&portal);
    if (error == ESP_OK && !portal.ipv4[0]) error = ESP_FAIL;
    ryz_provisioning_platform_event_t event = {
        .type = error == ESP_OK ? RYZ_PROVISIONING_PLATFORM_PORTAL_READY : RYZ_PROVISIONING_PLATFORM_FAILED,
        .error = error,
    };
    snprintf(event.ipv4, sizeof(event.ipv4), "%s", portal.ipv4);
    emit(event);
    return error;
}

esp_err_t ryz_provisioning_platform_clear_credentials(void)
{
    ++s_clear_count;
    if (s_clear_result != ESP_OK) return s_clear_result;
    memset(&s_credentials, 0, sizeof(s_credentials));
    s_has_credentials = false;
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_start_saved_sta(
    const ryz_provisioning_credentials_t *credentials)
{
    ++s_saved_scan_count;
    ryz_provisioning_snapshot_t view;
    /* This read would deadlock if the core retained its snapshot lock over IO. */
    assert(ryz_provisioning_get_snapshot(&view) == ESP_OK);
    assert(view.boot_phase == RYZ_PROVISIONING_BOOT_SCANNING);
    assert(!strcmp(view.sta_ssid, credentials->ssid));
    if (s_saved_scan_result != ESP_OK) return s_saved_scan_result;
    fake_platform_begin_connecting();
    return ryz_provisioning_platform_start_sta(credentials);
}
