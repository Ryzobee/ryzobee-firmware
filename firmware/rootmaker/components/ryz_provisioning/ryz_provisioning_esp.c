#include "ryz_provisioning_platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ryz_captive_dns.h"
#include "ryz_net_traffic.h"
#include "ryz_provisioning_generation.h"
#include "ryz_provisioning_init_plan.h"
#include "ryz_provisioning_portal.h"
#if defined(ESP_PLATFORM)
#include "ryz_diagnostics.h"
#include "lwip/sockets.h"
#endif

#define RYZ_NVS_NAMESPACE "ryz_prov"
#define RYZ_NVS_CREDENTIALS_KEY "sta_cred"
#define RYZ_NVS_AP_PASSWORD_KEY "ap_pass"
#define RYZ_NVS_ENABLED_KEY "wifi_on"
#define RYZ_CREDENTIALS_VERSION 1U
#define RYZ_AP_PASSWORD_LENGTH 12U
#define RYZ_PORTAL_BODY_MAX 384U
#define RYZ_CONNECT_QUEUE_LENGTH 1U
#define RYZ_PORTAL_CONNECT_DELAY_MS 250U
#define RYZ_LINK_SAMPLE_INTERVAL_US UINT64_C(1000000)
#define RYZ_LOCAL_SECRET_MAX_AGE_US UINT64_C(2500000)
#define RYZ_CONNECT_TIMEOUT_US UINT64_C(60000000)
#define RYZ_SCAN_MAX_ENTRIES 16U
#define RYZ_SCAN_MAX_CHANNELS 14U

#if defined(ESP_PLATFORM)
#define RYZ_PORTAL_WIFI_MODE WIFI_MODE_APSTA
#else
/* The host contract harness intentionally models the legacy AP-only radio;
 * target builds use APSTA so the browser can observe the complete flow. */
#define RYZ_PORTAL_WIFI_MODE WIFI_MODE_AP
#endif

typedef struct {
    uint32_t version;
    char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    char password[RYZ_PROVISIONING_PASSWORD_MAX_LENGTH + 1];
} stored_credentials_t;

typedef enum {
    NETWORK_MODE_STOPPED = 0,
    NETWORK_MODE_AP,
    NETWORK_MODE_STA,
} network_mode_t;

typedef enum {
    RYZ_SCAN_IDLE = 0,
    RYZ_SCAN_QUEUED,
    RYZ_SCAN_RUNNING,
    RYZ_SCAN_READY,
    RYZ_SCAN_FAILED,
} ryz_scan_state_t;

typedef struct {
    char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    uint8_t channel;
    int8_t rssi;
    uint8_t authmode;
} ryz_scan_record_t;

typedef enum {
    NETWORK_EVENT_STA_GOT_IP = 0,
    NETWORK_EVENT_STA_DISCONNECTED,
} network_event_type_t;

typedef struct {
    uint32_t generation;
    TickType_t due_tick;
    ryz_provisioning_credentials_t credentials;
} connect_command_t;

typedef struct {
    network_event_type_t type;
    uint32_t generation;
    char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    char ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
    uint8_t disconnect_reason;
} network_event_command_t;

static const char *TAG = "ryz_provisioning";
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_command_mux = portMUX_INITIALIZER_UNLOCKED;
static ryz_provisioning_platform_event_sink_t s_event_sink;
static SemaphoreHandle_t s_network_mutex;
static QueueHandle_t s_connect_queue;
static TaskHandle_t s_connect_task;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_http_server;
static ryz_captive_dns_handle_t s_dns_server;
static network_mode_t s_network_mode;
static bool s_wifi_started;
static bool s_connect_pending;
static bool s_cancel_requested;
static uint32_t s_cancel_generation;
static uint8_t s_reconnect_attempts;
static bool s_reconnect_pending;
static uint32_t s_generation;
/* Command-mux owned. Raw SDK disconnects may be old, but must conservatively
 * end a local hold even if the worker later observes a fast reassociation. */
static bool s_secret_revoke_pending;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static ryz_provisioning_init_plan_t s_init_plan;
static network_event_command_t s_network_event_mailbox;
static bool s_network_event_pending;
static bool s_network_event_saw_got_ip;
static uint32_t s_network_event_got_ip_generation;
static char s_ap_ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
static char s_ap_password[RYZ_PROVISIONING_PASSWORD_MAX_LENGTH + 1];
static ryz_provisioning_credentials_t s_sta_credentials;
static char s_captive_url[32];
static char s_portal_ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
static bool s_portal_session;
static uint64_t s_connect_started_us;
static uint64_t s_portal_started_us;

/* Scan state is independent of the public provisioning snapshot. The HTTP
 * handlers only enqueue/read this small cache; the worker is the sole owner
 * of esp_wifi_scan_* calls. */
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;
static ryz_scan_record_t s_scan_records[RYZ_SCAN_MAX_ENTRIES];
static uint16_t s_scan_count;
static uint16_t s_scan_total;
static uint8_t s_scan_channels[RYZ_SCAN_MAX_CHANNELS];
static uint8_t s_scan_channel_count;
static int8_t s_scan_rssi_min;
static int8_t s_scan_rssi_max;
static uint32_t s_scan_revision;
static uint64_t s_scan_started_us;
static uint64_t s_scan_completed_us;
static esp_err_t s_scan_error;
static ryz_scan_state_t s_scan_state;
static bool s_scan_requested;

#if defined(ESP_PLATFORM)
/* Kept out of the worker stack (which is intentionally small). The driver
 * releases its complete scan list when get_ap_records is called, even when
 * this bounded view only asks for the strongest 16 records. */
static wifi_ap_record_t s_scan_driver_records[RYZ_SCAN_MAX_ENTRIES];
#endif

static bool disconnect_is_credentials_failure(uint8_t reason);
/* Network-mutex owned. Only validated ONLINE observations establish this
 * binding; copied SDK events by themselves never do. The remembered identity
 * lets a failed sample invalidate exactly the last accepted connection. */
static struct {
    bool valid;
    uint32_t generation;
    char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    char ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
    uint64_t next_sample_us;
} s_link_binding;

/* Network-mutex owned. Configured credentials are retained only for this STA
 * configuration (including automatic retry), never exported through events.
 * The counter intentionally survives init rollback and never wraps/reuses. */
static uint64_t s_secret_epoch_counter;
static struct {
    bool valid;
    uint64_t epoch;
    uint64_t sampled_at_us;
    uint8_t bssid[6];
} s_local_secret;

static void copy_string(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0) {
        return;
    }
    size_t length = source == NULL ? 0 : strnlen(source, destination_size - 1);
    if (length > 0) {
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

static const char *scan_state_name(ryz_scan_state_t state)
{
    switch (state) {
    case RYZ_SCAN_QUEUED:
        return "queued";
    case RYZ_SCAN_RUNNING:
        return "running";
    case RYZ_SCAN_READY:
        return "ready";
    case RYZ_SCAN_FAILED:
        return "failed";
    case RYZ_SCAN_IDLE:
    default:
        return "idle";
    }
}

static void reset_scan_cache(void)
{
    portENTER_CRITICAL(&s_scan_mux);
    memset(s_scan_records, 0, sizeof(s_scan_records));
    memset(s_scan_channels, 0, sizeof(s_scan_channels));
    s_scan_count = 0;
    s_scan_total = 0;
    s_scan_channel_count = 0;
    s_scan_rssi_min = 0;
    s_scan_rssi_max = 0;
    s_scan_revision = 0;
    s_scan_started_us = 0;
    s_scan_completed_us = 0;
    s_scan_error = ESP_OK;
    s_scan_state = RYZ_SCAN_IDLE;
    s_scan_requested = false;
    portEXIT_CRITICAL(&s_scan_mux);
}

#if defined(ESP_PLATFORM)
static void request_scan(void)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    bool notify = false;
    portENTER_CRITICAL(&s_scan_mux);
    if (s_scan_state != RYZ_SCAN_RUNNING) {
        s_scan_state = RYZ_SCAN_QUEUED;
        s_scan_requested = true;
        s_scan_started_us = now;
        s_scan_completed_us = 0;
        s_scan_error = ESP_OK;
        ++s_scan_revision;
        notify = true;
    }
    portEXIT_CRITICAL(&s_scan_mux);
    if (notify && s_connect_task != NULL) {
        xTaskNotifyGive(s_connect_task);
    }
}

static bool request_cancel(void)
{
    bool accepted = false;
    portENTER_CRITICAL(&s_command_mux);
    /* Capture the generation at admission time. The worker will invalidate
     * this generation before touching the radio, so a late disconnect/IP
     * callback cannot resurrect the cancelled attempt. */
    if (s_portal_session &&
        (s_connect_pending || s_network_mode == NETWORK_MODE_STA)) {
        s_cancel_generation = s_generation;
        s_cancel_requested = true;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_command_mux);
    if (accepted && s_connect_task != NULL) {
        xTaskNotifyGive(s_connect_task);
    }
    return accepted;
}
#endif

static bool take_scan_request(void)
{
    bool requested;
    portENTER_CRITICAL(&s_scan_mux);
    requested = s_scan_requested;
    if (requested) {
        s_scan_requested = false;
        s_scan_state = RYZ_SCAN_RUNNING;
        s_scan_started_us = (uint64_t)esp_timer_get_time();
        s_scan_completed_us = 0;
        s_scan_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_scan_mux);
    return requested;
}

#if defined(ESP_PLATFORM)
static bool take_cancel_request(uint32_t *out_generation)
{
    if (out_generation == NULL) return false;
    bool requested = false;
    portENTER_CRITICAL(&s_command_mux);
    if (s_cancel_requested) {
        requested = true;
        *out_generation = s_cancel_generation;
        s_cancel_requested = false;
    }
    portEXIT_CRITICAL(&s_command_mux);
    return requested;
}
#endif

static void finish_scan(
    esp_err_t error,
    const ryz_scan_record_t *records,
    uint16_t count,
    uint16_t total,
    const uint8_t *channels,
    uint8_t channel_count,
    int8_t rssi_min,
    int8_t rssi_max)
{
    uint64_t completed = (uint64_t)esp_timer_get_time();
    if (count > RYZ_SCAN_MAX_ENTRIES) count = RYZ_SCAN_MAX_ENTRIES;
    if (total < count) total = count;
    if (channel_count > RYZ_SCAN_MAX_CHANNELS) channel_count = RYZ_SCAN_MAX_CHANNELS;
    portENTER_CRITICAL(&s_scan_mux);
    memset(s_scan_records, 0, sizeof(s_scan_records));
    memset(s_scan_channels, 0, sizeof(s_scan_channels));
    if (records != NULL && count != 0) {
        memcpy(s_scan_records, records, count * sizeof(s_scan_records[0]));
    }
    if (channels != NULL && channel_count != 0) {
        memcpy(s_scan_channels, channels, channel_count);
    }
    s_scan_count = count;
    s_scan_total = total;
    s_scan_channel_count = channel_count;
    s_scan_rssi_min = rssi_min;
    s_scan_rssi_max = rssi_max;
    s_scan_completed_us = completed;
    s_scan_error = error;
    s_scan_state = error == ESP_OK ? RYZ_SCAN_READY : RYZ_SCAN_FAILED;
    ++s_scan_revision;
    portEXIT_CRITICAL(&s_scan_mux);
}

static bool portal_session_active(void)
{
    portENTER_CRITICAL(&s_command_mux);
    bool active = s_portal_session && s_http_server != NULL;
    portEXIT_CRITICAL(&s_command_mux);
    return active;
}

static bool portal_accepting_credentials_locked(void)
{
#if defined(ESP_PLATFORM)
    return s_portal_session &&
        (s_network_mode == NETWORK_MODE_AP || s_network_mode == NETWORK_MODE_STOPPED);
#else
    return s_network_mode == NETWORK_MODE_AP;
#endif
}

static const char *portal_error_code(esp_err_t error, int32_t detail)
{
    if (error == ESP_OK) return "";
    if (detail >= 0 && detail <= UINT8_MAX &&
        disconnect_is_credentials_failure((uint8_t)detail)) {
        return "E_WIFI_AUTH";
    }
    if (error == ESP_ERR_TIMEOUT) return "E_WIFI_TIMEOUT";
    if (error == ESP_ERR_WIFI_NOT_CONNECT) return "E_WIFI_NOT_FOUND";
    return "E_WIFI_CONNECT";
}

static const char *scan_auth_name(uint8_t authmode)
{
    return authmode == (uint8_t)WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
}

static void wipe_secret(void *buffer, size_t size)
{
    volatile unsigned char *bytes = buffer;
    while (size--) *bytes++ = 0;
}

static void revoke_local_secret_locked(void)
{
    memset(&s_local_secret, 0, sizeof(s_local_secret));
}

static bool local_secret_revoke_pending(void)
{
    portENTER_CRITICAL(&s_command_mux);
    bool pending = s_secret_revoke_pending;
    portEXIT_CRITICAL(&s_command_mux);
    return pending;
}

static bool local_secret_fresh_locked(uint64_t now)
{
    if (s_local_secret.valid &&
        (local_secret_revoke_pending() || !s_link_binding.valid || s_network_mode != NETWORK_MODE_STA ||
         s_link_binding.generation != s_generation ||
         now < s_local_secret.sampled_at_us ||
         now - s_local_secret.sampled_at_us > RYZ_LOCAL_SECRET_MAX_AGE_US)) {
        revoke_local_secret_locked();
    }
    return s_local_secret.valid;
}

esp_err_t ryz_provisioning_platform_local_secret_try_get(ryz_provisioning_local_secret_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (s_network_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_network_mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (local_secret_fresh_locked((uint64_t)esp_timer_get_time())) {
        out->available = true;
        out->epoch = s_local_secret.epoch;
    }
    xSemaphoreGive(s_network_mutex);
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_local_secret_try_copy(uint64_t expected_epoch, char *out, size_t cap)
{
    if (out != NULL && cap != 0) wipe_secret(out, cap);
    if (out == NULL || cap == 0 || expected_epoch == 0) return ESP_ERR_INVALID_ARG;
    if (s_network_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_network_mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (local_secret_fresh_locked((uint64_t)esp_timer_get_time()) &&
        s_local_secret.epoch == expected_epoch) {
        size_t length = strlen(s_sta_credentials.password);
        error = cap > length ? ESP_OK : ESP_ERR_INVALID_SIZE;
        if (error == ESP_OK) memcpy(out, s_sta_credentials.password, length + 1U);
    }
    xSemaphoreGive(s_network_mutex);
    return error;
}

static void observe_local_secret_locked(const uint8_t bssid[6], uint64_t now)
{
    if (local_secret_revoke_pending()) {
        revoke_local_secret_locked();
        return;
    }
    if (local_secret_fresh_locked(now) &&
        memcmp(s_local_secret.bssid, bssid, sizeof(s_local_secret.bssid)) != 0)
        revoke_local_secret_locked();
    if (!s_local_secret.valid) {
        if (s_secret_epoch_counter == UINT64_MAX) return; /* Permanent fail closed. */
        s_local_secret.epoch = ++s_secret_epoch_counter;
        memcpy(s_local_secret.bssid, bssid, sizeof(s_local_secret.bssid));
        s_local_secret.valid = true;
    }
    s_local_secret.sampled_at_us = now;
}

static ryz_provisioning_generation_state_t command_state_locked(void)
{
    return (ryz_provisioning_generation_state_t){
        .generation = s_generation,
        .connect_pending = s_connect_pending,
        .reconnect_attempts = s_reconnect_attempts,
        .reconnect_pending = s_reconnect_pending,
    };
}

static void store_command_state_locked(
    const ryz_provisioning_generation_state_t *state)
{
    s_generation = state->generation;
    s_connect_pending = state->connect_pending;
    s_reconnect_attempts = state->reconnect_attempts;
    s_reconnect_pending = state->reconnect_pending;
}

static void begin_network_transition_locked(void)
{
    ryz_net_traffic_revoke();
    revoke_local_secret_locked();
    s_link_binding.valid = false;
    s_link_binding.ssid[0] = '\0';
    s_link_binding.ipv4[0] = '\0';
    ryz_provisioning_generation_state_t state = command_state_locked();
    ryz_provisioning_begin_generation(&state);
    store_command_state_locked(&state);
    s_network_mode = NETWORK_MODE_STOPPED;
    s_cancel_requested = false;
    s_cancel_generation = 0;
    s_network_event_pending = false;
    s_network_event_saw_got_ip = false;
}

static bool disconnect_is_credentials_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_GROUP_CIPHER_INVALID:
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
    case WIFI_REASON_AKMP_INVALID:
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
    case WIFI_REASON_INVALID_RSN_IE_CAP:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
    case WIFI_REASON_BAD_CIPHER_OR_AKM:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return true;
    default:
        return false;
    }
}

static bool is_valid_credentials(const ryz_provisioning_credentials_t *credentials)
{
    if (credentials == NULL) {
        return false;
    }
    size_t ssid_length = strnlen(credentials->ssid, sizeof(credentials->ssid));
    size_t password_length = strnlen(credentials->password, sizeof(credentials->password));
    if (ssid_length == 0 || ssid_length > RYZ_PROVISIONING_SSID_MAX_LENGTH ||
        password_length > RYZ_PROVISIONING_PASSWORD_MAX_LENGTH ||
        (password_length > 0 && password_length < 8)) {
        return false;
    }
    for (size_t index = 0; index < password_length; ++index) {
        unsigned char character = (unsigned char)credentials->password[index];
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
    }
    return true;
}

static void emit_event(
    ryz_provisioning_platform_event_type_t type,
    esp_err_t error,
    int32_t detail,
    const char *ssid,
    const char *ipv4)
{
    if (s_event_sink == NULL) {
        return;
    }
    ryz_provisioning_platform_event_t event = {
        .type = type,
        .error = error,
        .detail = detail,
    };
    copy_string(event.ssid, sizeof(event.ssid), ssid);
    copy_string(event.ipv4, sizeof(event.ipv4), ipv4);
    s_event_sink(&event);
}

void ryz_provisioning_platform_snapshot_lock(void)
{
    portENTER_CRITICAL(&s_snapshot_mux);
}

void ryz_provisioning_platform_snapshot_unlock(void)
{
    portEXIT_CRITICAL(&s_snapshot_mux);
}

static esp_err_t save_credentials(const ryz_provisioning_credentials_t *credentials)
{
    if (!is_valid_credentials(credentials)) {
        return ESP_ERR_INVALID_ARG;
    }

    stored_credentials_t stored = {
        .version = RYZ_CREDENTIALS_VERSION,
    };
    copy_string(stored.ssid, sizeof(stored.ssid), credentials->ssid);
    copy_string(stored.password, sizeof(stored.password), credentials->password);

    nvs_handle_t handle;
    esp_err_t error = nvs_open(RYZ_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, RYZ_NVS_CREDENTIALS_KEY, &stored, sizeof(stored));
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t ryz_provisioning_platform_load_enabled(bool *out_enabled)
{
    if (out_enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_enabled = false;

    nvs_handle_t handle;
    esp_err_t error = nvs_open(RYZ_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : error;
    }
    uint8_t stored = 0;
    error = nvs_get_u8(handle, RYZ_NVS_ENABLED_KEY, &stored);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (stored > 1U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_enabled = stored == 1U;
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_save_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(RYZ_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_u8(handle, RYZ_NVS_ENABLED_KEY, enabled ? 1U : 0U);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t ryz_provisioning_platform_load_credentials(
    ryz_provisioning_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(credentials, 0, sizeof(*credentials));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(RYZ_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : error;
    }

    stored_credentials_t stored = {0};
    size_t stored_size = sizeof(stored);
    error = nvs_get_blob(handle, RYZ_NVS_CREDENTIALS_KEY, &stored, &stored_size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (stored_size != sizeof(stored) || stored.version != RYZ_CREDENTIALS_VERSION) {
        return ESP_ERR_NOT_FOUND;
    }

    copy_string(credentials->ssid, sizeof(credentials->ssid), stored.ssid);
    copy_string(credentials->password, sizeof(credentials->password), stored.password);
    return is_valid_credentials(credentials) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t clear_credentials_record(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(RYZ_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_key(handle, RYZ_NVS_CREDENTIALS_KEY);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t ryz_provisioning_platform_clear_credentials(void)
{
    if (s_network_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_command_mux);
    ryz_provisioning_generation_state_t state = command_state_locked();
    ryz_provisioning_begin_generation(&state);
    store_command_state_locked(&state);
    portEXIT_CRITICAL(&s_command_mux);
    esp_err_t error = clear_credentials_record();
    xSemaphoreGive(s_network_mutex);
    return error;
}

static esp_err_t load_or_create_ap_password(char *password, size_t password_size)
{
    if (password == NULL || password_size <= RYZ_AP_PASSWORD_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t error = nvs_open(RYZ_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    size_t stored_size = password_size;
    error = nvs_get_str(handle, RYZ_NVS_AP_PASSWORD_KEY, password, &stored_size);
    if (error == ESP_OK && strlen(password) == RYZ_AP_PASSWORD_LENGTH) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (error != ESP_ERR_NVS_NOT_FOUND && error != ESP_ERR_NVS_INVALID_LENGTH &&
        error != ESP_OK) {
        nvs_close(handle);
        return error;
    }

    static const char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint8_t random_bytes[RYZ_AP_PASSWORD_LENGTH];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t index = 0; index < RYZ_AP_PASSWORD_LENGTH; ++index) {
        password[index] = ALPHABET[random_bytes[index] & 31U];
    }
    password[RYZ_AP_PASSWORD_LENGTH] = '\0';

    error = nvs_set_str(handle, RYZ_NVS_AP_PASSWORD_KEY, password);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t ryz_provisioning_platform_get_ap_identity(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size)
{
    if (ssid == NULL || ssid_size < sizeof("RYZOBEE-0000") || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    esp_err_t error = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (error != ESP_OK) {
        return error;
    }
    int written = snprintf(ssid, ssid_size, "RYZOBEE-%02X%02X", mac[4], mac[5]);
    if (written < 0 || (size_t)written >= ssid_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    error = load_or_create_ap_password(password, password_size);
    if (error != ESP_OK) {
        return error;
    }
    copy_string(s_ap_ssid, sizeof(s_ap_ssid), ssid);
    copy_string(s_ap_password, sizeof(s_ap_password), password);
    return ESP_OK;
}

static esp_err_t stop_portal_services_locked(void)
{
    esp_err_t error = ESP_OK;
    if (s_dns_server != NULL) {
        error = ryz_captive_dns_stop(s_dns_server);
        if (error == ESP_OK) {
            s_dns_server = NULL;
        }
    }
    if (s_http_server != NULL) {
        /* IDF httpd_stop can fail to send its shutdown message without
         * consuming the handle. Retain ownership until a successful retry.
         * Its wait is not deadline-bounded; only the trusted network owner
         * may call this, never an HTTP handler or display/event task. */
        esp_err_t http_error = httpd_stop(s_http_server);
        if (http_error == ESP_OK) {
            s_http_server = NULL;
        } else if (error == ESP_OK) {
            error = http_error;
        }
    }
    return error;
}

static esp_err_t stop_wifi_locked(void)
{
    if (!s_wifi_started) {
        return ESP_OK;
    }
    esp_err_t error = esp_wifi_stop();
    if (error == ESP_OK || error == ESP_ERR_WIFI_NOT_STARTED) {
        s_wifi_started = false;
        return ESP_OK;
    }
    return error;
}

static esp_err_t stop_network_locked(void)
{
    /* Revoke admissions before potentially waiting for existing handlers.
     * An HTTP handler never waits for this mutex (it returns 409), so the
     * owner holding it may safely ask httpd_stop to join that task. */
    portENTER_CRITICAL(&s_command_mux);
    begin_network_transition_locked();
    s_portal_session = false;
    s_cancel_requested = false;
    s_cancel_generation = 0;
    s_portal_ipv4[0] = '\0';
    s_connect_started_us = 0;
    s_portal_started_us = 0;
    portEXIT_CRITICAL(&s_command_mux);
    reset_scan_cache();
    wipe_secret(&s_sta_credentials, sizeof(s_sta_credentials));
    if (s_connect_task != NULL) {
        xTaskNotifyGive(s_connect_task);
    }
    esp_err_t error = stop_portal_services_locked();
    /* Still stop the radio if a portal cleanup step fails. A retained
     * portal handle means NOT confirmed OFF, even when the radio is off. */
    esp_err_t wifi_error = stop_wifi_locked();
    return error != ESP_OK ? error : wifi_error;
}

esp_err_t ryz_provisioning_platform_stop(void)
{
    if (s_network_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    esp_err_t error = stop_network_locked();
    xSemaphoreGive(s_network_mutex);
    return error;
}

static const char *state_name(ryz_provisioning_state_t state)
{
    switch (state) {
    case RYZ_PROVISIONING_UNCONFIGURED:
        return "UNCONFIGURED";
    case RYZ_PROVISIONING_AP_STARTING:
        return "AP_STARTING";
    case RYZ_PROVISIONING_AP_READY:
        return "AP_READY";
    case RYZ_PROVISIONING_CREDENTIALS_RECEIVED:
        return "CREDENTIALS_RECEIVED";
    case RYZ_PROVISIONING_CONNECTING:
        return "CONNECTING";
    case RYZ_PROVISIONING_ONLINE:
        return "ONLINE";
    case RYZ_PROVISIONING_FAILED:
        return "FAILED";
    case RYZ_PROVISIONING_STOPPING:
        return "STOPPING";
    case RYZ_PROVISIONING_OFF:
        return "OFF";
    default:
        return "UNKNOWN";
    }
}

static void set_common_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(
        request,
        "Content-Security-Policy",
        "default-src 'none'; style-src 'self' 'unsafe-inline'; font-src data:; script-src 'unsafe-inline'; "
        "connect-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
}

static esp_err_t web_css_get_handler(httpd_req_t *request)
{
    set_common_headers(request);
    httpd_resp_set_type(request, "text/css; charset=utf-8");
    return httpd_resp_send(request, ryz_provisioning_web_css, HTTPD_RESP_USE_STRLEN);
}

bool ryz_provisioning_diagnostics_ready(void)
{
    if (s_network_mutex == NULL || xSemaphoreTake(s_network_mutex, 0) != pdTRUE) return false;
    bool ready = s_wifi_started && s_ap_netif != NULL && strlen(s_ap_password) >= 8;
    portENTER_CRITICAL(&s_command_mux);
    ready = ready && s_portal_session && s_http_server != NULL && s_portal_ipv4[0] != '\0';
    portEXIT_CRITICAL(&s_command_mux);
    xSemaphoreGive(s_network_mutex);
    return ready;
}

#if defined(ESP_PLATFORM)
/* The HTTP server also binds the STA interface in APSTA mode. An AP password
 * alone is therefore not an access guard: verify the actual destination and
 * peer subnet, and only expose reports while the existing protected AP lives.
 * Never create an AP, change Wi-Fi settings, or publish a latest-report route. */
static bool diagnostics_ap_request_allowed(httpd_req_t *request)
{
    struct sockaddr_in local = {0}, peer = {0};
    socklen_t local_length = sizeof(local), peer_length = sizeof(peer);
    int socket = httpd_req_to_sockfd(request);
    if (socket < 0 || getsockname(socket, (struct sockaddr *)&local, &local_length) != 0 ||
        getpeername(socket, (struct sockaddr *)&peer, &peer_length) != 0 ||
        local.sin_family != AF_INET || peer.sin_family != AF_INET) return false;
    if (s_network_mutex == NULL || xSemaphoreTake(s_network_mutex, 0) != pdTRUE) return false;
    esp_netif_ip_info_t ap = {0};
    bool active = s_wifi_started && s_ap_netif != NULL && strlen(s_ap_password) >= 8;
    portENTER_CRITICAL(&s_command_mux);
    active = active && s_portal_session && s_http_server != NULL;
    portEXIT_CRITICAL(&s_command_mux);
    bool allowed = active && esp_netif_get_ip_info(s_ap_netif, &ap) == ESP_OK &&
        ap.ip.addr != 0 && ap.netmask.addr != 0 && local.sin_addr.s_addr == ap.ip.addr &&
        (peer.sin_addr.s_addr & ap.netmask.addr) == (ap.ip.addr & ap.netmask.addr);
    xSemaphoreGive(s_network_mutex);
    return allowed;
}

static esp_err_t diagnostics_gone(httpd_req_t *request)
{
    set_common_headers(request);
    httpd_resp_set_status(request, "410 Gone");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, "{\"error\":\"report_unavailable\"}", HTTPD_RESP_USE_STRLEN);
}

static bool diagnostics_read_target(httpd_req_t *request, const char *prefix,
                                    ryz_diagnostics_snapshot_t *snapshot)
{
    const char *uri = request->uri;
    size_t start = strlen(prefix);
    if (strncmp(uri, prefix, start) != 0) return false;
    size_t remaining = strlen(uri + start);
    if (remaining < 8 || (uri[start + 8] != '?' && uri[start + 8] != '\0')) return false;
    char id[9], capability[33], query[80];
    memcpy(id, uri + start, 8); id[8] = '\0';
    /* One exact query prevents duplicate/ambiguous capability interpretations. */
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        strncmp(query, "cap=", 4) != 0 || strlen(query) != 36) return false;
    memcpy(capability, query + 4, 33);
    return ryz_diagnostics_snapshot(id, capability, snapshot) == ESP_OK;
}

static esp_err_t diagnostics_page_get_handler(httpd_req_t *request)
{
    if (!diagnostics_ap_request_allowed(request)) return diagnostics_gone(request);
    /* The shell contains no report or capability. Even an expired id can show
     * the designed unavailable state; JSON access is separately revalidated. */
    set_common_headers(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, ryz_diagnostics_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t diagnostics_json_get_handler(httpd_req_t *request)
{
    if (!diagnostics_ap_request_allowed(request)) return diagnostics_gone(request);
    ryz_diagnostics_snapshot_t snapshot;
    if (!diagnostics_read_target(request, "/api/diagnostics/", &snapshot)) return diagnostics_gone(request);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
#define TEXT(field) cJSON_AddStringToObject(root, #field, snapshot.field)
    TEXT(id); TEXT(phase); TEXT(code); TEXT(summary); TEXT(source_file);
    TEXT(job_id); TEXT(firmware_version); TEXT(error); TEXT(output);
#undef TEXT
    if (snapshot.line >= 0) cJSON_AddNumberToObject(root, "line", snapshot.line);
    else cJSON_AddNullToObject(root, "line");
    if (snapshot.duration_ms >= 0) cJSON_AddNumberToObject(root, "duration_ms", (double)snapshot.duration_ms);
    else cJSON_AddNullToObject(root, "duration_ms");
    if (snapshot.peak_bytes >= 0) cJSON_AddNumberToObject(root, "peak_bytes", (double)snapshot.peak_bytes);
    else cJSON_AddNullToObject(root, "peak_bytes");
    cJSON_AddBoolToObject(root, "lua_ok", snapshot.lua_ok);
    cJSON_AddBoolToObject(root, "ok", snapshot.ok);
    if (snapshot.cleanup_ok >= 0) cJSON_AddBoolToObject(root, "cleanup_ok", snapshot.cleanup_ok != 0);
    else cJSON_AddNullToObject(root, "cleanup_ok");
    cJSON_AddBoolToObject(root, "error_truncated", snapshot.error_truncated);
    cJSON_AddBoolToObject(root, "output_truncated", snapshot.output_truncated);
    cJSON_AddNullToObject(root, "trace");
    cJSON_AddBoolToObject(root, "text_sanitized", snapshot.text_sanitized);
    if (cJSON_GetArraySize(root) != 19) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    set_common_headers(request);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    esp_err_t error = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return error;
}
#endif

static esp_err_t root_get_handler(httpd_req_t *request)
{
    set_common_headers(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(
        request,
        ryz_provisioning_portal_html,
        HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    ryz_provisioning_snapshot_t snapshot;
    esp_err_t error = ryz_provisioning_get_snapshot(&snapshot);
    if (error != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "state unavailable");
    }

    ryz_scan_record_t scan_records[RYZ_SCAN_MAX_ENTRIES];
    uint8_t scan_channels[RYZ_SCAN_MAX_CHANNELS];
    uint16_t scan_count;
    uint16_t scan_total;
    uint8_t scan_channel_count;
    int8_t scan_rssi_min;
    int8_t scan_rssi_max;
    uint32_t scan_revision;
    uint64_t scan_started_us;
    uint64_t scan_completed_us;
    esp_err_t scan_error;
    ryz_scan_state_t scan_state;
    portENTER_CRITICAL(&s_scan_mux);
    memcpy(scan_records, s_scan_records, sizeof(scan_records));
    memcpy(scan_channels, s_scan_channels, sizeof(scan_channels));
    scan_count = s_scan_count;
    scan_total = s_scan_total;
    scan_channel_count = s_scan_channel_count;
    scan_rssi_min = s_scan_rssi_min;
    scan_rssi_max = s_scan_rssi_max;
    scan_revision = s_scan_revision;
    scan_started_us = s_scan_started_us;
    scan_completed_us = s_scan_completed_us;
    scan_error = s_scan_error;
    scan_state = s_scan_state;
    portEXIT_CRITICAL(&s_scan_mux);

    char portal_ip[sizeof(s_portal_ipv4)] = {0};
    char session[8] = {0};
    bool portal_active;
    uint64_t connect_started_us;
    uint64_t portal_started_us;
    portENTER_CRITICAL(&s_command_mux);
    portal_active = s_portal_session && s_http_server != NULL;
    copy_string(portal_ip, sizeof(portal_ip), s_portal_ipv4);
    connect_started_us = s_connect_started_us;
    portal_started_us = s_portal_started_us;
    portEXIT_CRITICAL(&s_command_mux);
    if (!portal_ip[0]) copy_string(portal_ip, sizeof(portal_ip), snapshot.portal_ip);
    const char *suffix = strrchr(snapshot.ap_ssid, '-');
    suffix = suffix != NULL ? suffix + 1 : snapshot.ap_ssid;
    size_t suffix_length = strlen(suffix);
    if (suffix_length > 4) {
        suffix += suffix_length - 4;
        suffix_length = 4;
    }
    memcpy(session, "RZB-", 4);
    if (suffix_length > sizeof(session) - 5) suffix_length = sizeof(session) - 5;
    memcpy(session + 4, suffix, suffix_length);
    session[4 + suffix_length] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    cJSON_AddStringToObject(root, "state", state_name(snapshot.state));
    cJSON_AddNumberToObject(root, "revision", snapshot.revision);
    cJSON_AddStringToObject(root, "ap_ssid", snapshot.ap_ssid);
    cJSON_AddStringToObject(root, "portal_ip", portal_ip);
    cJSON_AddBoolToObject(root, "portal_active", portal_active);
    cJSON_AddStringToObject(root, "session", session);
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t portal_session_elapsed_s = 0;
    if (portal_started_us != 0 && now_us >= portal_started_us) {
        portal_session_elapsed_s = (uint32_t)((now_us - portal_started_us) / UINT64_C(1000000));
    }
    cJSON_AddNumberToObject(root, "portal_session_elapsed_s", portal_session_elapsed_s);
    cJSON_AddBoolToObject(root, "credentials_stored", snapshot.credentials_stored);
    cJSON_AddStringToObject(root, "sta_ssid", snapshot.sta_ssid);
    cJSON_AddStringToObject(root, "ipv4", snapshot.ipv4);
    cJSON_AddStringToObject(
        root,
        "error_code",
        portal_error_code(snapshot.last_error, snapshot.detail));
    cJSON_AddNumberToObject(root, "error_detail", snapshot.detail);

    uint32_t connect_progress = 0;
    uint32_t connect_timeout_remaining_s = 0;
    if (snapshot.state == RYZ_PROVISIONING_ONLINE) {
        connect_progress = 100;
    } else if ((snapshot.state == RYZ_PROVISIONING_CONNECTING ||
                snapshot.state == RYZ_PROVISIONING_CREDENTIALS_RECEIVED) &&
               connect_started_us != 0) {
        uint64_t elapsed = now_us >= connect_started_us ? now_us - connect_started_us : 0;
        uint64_t scaled = elapsed >= RYZ_CONNECT_TIMEOUT_US
            ? 85U : (elapsed * 85U) / RYZ_CONNECT_TIMEOUT_US;
        connect_progress = 10U + (uint32_t)scaled;
        uint64_t remaining = elapsed >= RYZ_CONNECT_TIMEOUT_US
            ? 0 : RYZ_CONNECT_TIMEOUT_US - elapsed;
        connect_timeout_remaining_s = (uint32_t)((remaining + UINT64_C(999999)) / UINT64_C(1000000));
    }
    cJSON_AddNumberToObject(root, "connect_progress", connect_progress);
    cJSON_AddNumberToObject(root, "connect_timeout_remaining_s", connect_timeout_remaining_s);

    cJSON_AddStringToObject(root, "scan_state", scan_state_name(scan_state));
    cJSON_AddNumberToObject(root, "scan_revision", scan_revision);
    cJSON_AddNumberToObject(root, "scan_count", scan_count);
    cJSON_AddNumberToObject(root, "scan_total", scan_total);
    uint64_t scan_elapsed_us = scan_completed_us > scan_started_us
        ? scan_completed_us - scan_started_us : 0;
    cJSON_AddNumberToObject(root, "scan_elapsed_ms", (double)(scan_elapsed_us / 1000U));
    if (scan_error != ESP_OK && scan_state == RYZ_SCAN_FAILED) {
        cJSON_AddStringToObject(root, "scan_error", esp_err_to_name(scan_error));
    }
    cJSON *channel_array = cJSON_AddArrayToObject(root, "scan_channels");
    if (channel_array != NULL) {
        for (uint8_t index = 0; index < scan_channel_count; ++index) {
            cJSON *channel = cJSON_CreateNumber(scan_channels[index]);
            if (channel != NULL) cJSON_AddItemToArray(channel_array, channel);
        }
    }
    if (scan_count != 0) {
        cJSON_AddNumberToObject(root, "scan_rssi_min", scan_rssi_min);
        cJSON_AddNumberToObject(root, "scan_rssi_max", scan_rssi_max);
    }
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    if (networks != NULL) {
        for (uint16_t index = 0; index < scan_count; ++index) {
            cJSON *network = cJSON_CreateObject();
            if (network == NULL) continue;
            cJSON_AddStringToObject(network, "ssid", scan_records[index].ssid);
            cJSON_AddStringToObject(network, "auth", scan_auth_name(scan_records[index].authmode));
            cJSON_AddNumberToObject(network, "channel", scan_records[index].channel);
            cJSON_AddNumberToObject(network, "rssi", scan_records[index].rssi);
            cJSON_AddItemToArray(networks, network);
        }
    }
    if (snapshot.link.rssi_valid) {
        cJSON_AddNumberToObject(root, "ap_rssi_dbm", snapshot.link.rssi_dbm);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    set_common_headers(request);
    httpd_resp_set_type(request, "application/json");
    error = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return error;
}

static esp_err_t send_json_error(
    httpd_req_t *request,
    const char *status,
    const char *message)
{
    httpd_resp_set_status(request, status);
    set_common_headers(request);
    httpd_resp_set_type(request, "application/json");
    char response[160];
    snprintf(response, sizeof(response), "{\"error\":\"%s\"}", message);
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_request_body(httpd_req_t *request, char *body, size_t body_size)
{
    if (request->content_len <= 0 || (size_t)request->content_len >= body_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int result = httpd_req_recv(
            request,
            body + received,
            (size_t)request->content_len - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    return ESP_OK;
}

static bool release_pending_connect_locked(uint32_t generation)
{
    portENTER_CRITICAL(&s_command_mux);
    ryz_provisioning_generation_state_t state = command_state_locked();
    bool released = ryz_provisioning_release_connect_command(
        &state,
        portal_accepting_credentials_locked(),
        generation);
    if (released) {
        store_command_state_locked(&state);
    }
    portEXIT_CRITICAL(&s_command_mux);
    return released;
}

static esp_err_t provision_post_handler(httpd_req_t *request)
{
    char body[RYZ_PORTAL_BODY_MAX + 1];
    if (read_request_body(request, body, sizeof(body)) != ESP_OK) {
        return send_json_error(request, "400 Bad Request", "invalid request body");
    }

    cJSON *root = cJSON_ParseWithLength(body, strlen(body));
    const cJSON *ssid_item = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password_item = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *ssid = cJSON_GetStringValue(ssid_item);
    const char *password = cJSON_GetStringValue(password_item);
    if (ssid == NULL || password == NULL) {
        cJSON_Delete(root);
        return send_json_error(request, "400 Bad Request", "ssid and password are required");
    }

    size_t ssid_input_length = strlen(ssid);
    size_t password_input_length = strlen(password);
    ryz_provisioning_credentials_t credentials = {0};
    copy_string(credentials.ssid, sizeof(credentials.ssid), ssid);
    copy_string(credentials.password, sizeof(credentials.password), password);
    cJSON_Delete(root);
    if (!is_valid_credentials(&credentials) ||
        ssid_input_length != strlen(credentials.ssid) ||
        password_input_length != strlen(credentials.password)) {
        return send_json_error(request, "400 Bad Request", "invalid Wi-Fi credentials");
    }

    connect_command_t command = {
        .credentials = credentials,
    };
    /* Never block an HTTP server task on the network lock: a concurrent
     * transition may hold that lock while httpd_stop() waits for this handler
     * to return. The client can safely retry after the transition settles. */
    if (xSemaphoreTake(s_network_mutex, 0) != pdTRUE) {
        return send_json_error(
            request,
            "409 Conflict",
            "network transition in progress");
    }
    bool conflict;
    portENTER_CRITICAL(&s_command_mux);
    if (s_connect_pending || !portal_accepting_credentials_locked()) {
        conflict = true;
    } else {
        conflict = false;
        s_connect_pending = true;
        command.generation = s_generation;
    }
    portEXIT_CRITICAL(&s_command_mux);
    if (conflict) {
        xSemaphoreGive(s_network_mutex);
        return send_json_error(request, "409 Conflict", "connection already in progress");
    }

    esp_err_t error = save_credentials(&credentials);
    if (error != ESP_OK) {
        release_pending_connect_locked(command.generation);
        xSemaphoreGive(s_network_mutex);
        return send_json_error(request, "500 Internal Server Error", "could not store credentials");
    }

    command.due_tick =
        xTaskGetTickCount() + pdMS_TO_TICKS(RYZ_PORTAL_CONNECT_DELAY_MS);
    portENTER_CRITICAL(&s_command_mux);
    bool current = s_connect_pending && command.generation == s_generation &&
                   portal_accepting_credentials_locked();
    portEXIT_CRITICAL(&s_command_mux);
    /* The queue has length one. If a prior generation left a stale command
     * behind, current ownership proves it is safe to replace that command. */
    bool queued = current &&
                  xQueueOverwrite(s_connect_queue, &command) == pdTRUE;
    if (!queued) {
        bool owned = release_pending_connect_locked(command.generation);
        esp_err_t rollback_error = ESP_OK;
        if (owned) {
            rollback_error = clear_credentials_record();
        }
        xSemaphoreGive(s_network_mutex);
        return send_json_error(
            request,
            rollback_error != ESP_OK ? "500 Internal Server Error" :
            current ? "503 Service Unavailable" : "409 Conflict",
            rollback_error != ESP_OK ? "could not roll back credentials" :
            current ? "connection queue is unavailable" :
                      "provisioning state changed");
    }

    emit_event(
        RYZ_PROVISIONING_PLATFORM_CREDENTIALS_RECEIVED,
        ESP_OK,
        0,
        credentials.ssid,
        NULL);
    portENTER_CRITICAL(&s_command_mux);
    s_connect_started_us = (uint64_t)esp_timer_get_time();
    portEXIT_CRITICAL(&s_command_mux);
    xTaskNotifyGive(s_connect_task);
    xSemaphoreGive(s_network_mutex);
    set_common_headers(request);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

#if defined(ESP_PLATFORM)
static bool scan_has_ssid(
    const ryz_scan_record_t *records,
    uint16_t count,
    const char *ssid)
{
    for (uint16_t index = 0; index < count; ++index) {
        if (strcmp(records[index].ssid, ssid) == 0) return true;
    }
    return false;
}

static bool scan_has_channel(const uint8_t *channels, uint8_t count, uint8_t channel)
{
    for (uint8_t index = 0; index < count; ++index) {
        if (channels[index] == channel) return true;
    }
    return false;
}
#endif

static void perform_scan(void)
{
    ryz_scan_record_t records[RYZ_SCAN_MAX_ENTRIES] = {0};
    uint8_t channels[RYZ_SCAN_MAX_CHANNELS] = {0};
    uint16_t record_count = 0;
    uint16_t total_count = 0;
    uint8_t channel_count = 0;
    int8_t rssi_min = 0;
    int8_t rssi_max = 0;
    esp_err_t error = ESP_OK;

#if defined(ESP_PLATFORM)
    wifi_scan_config_t config = {0};
    config.channel = 0; /* Use the 2.4 GHz bitmap below, never 5 GHz. */
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 0;
    config.scan_time.active.max = 100;
    config.home_chan_dwell_time = 30;
    config.channel_bitmap.ghz_2_channels = 0x7FFE; /* channels 1..14 */
    config.channel_bitmap.ghz_5_channels = 0;
    config.coex_background_scan = true;

    error = esp_wifi_scan_start(&config, true);
    if (error == ESP_OK) {
        esp_err_t count_error = esp_wifi_scan_get_ap_num(&total_count);
        if (count_error != ESP_OK) {
            error = count_error;
            (void)esp_wifi_clear_ap_list();
        }
    }
    if (error == ESP_OK) {
        /* This single bounded read also releases the driver's complete
         * dynamically allocated AP list, as required by ESP-IDF. */
        uint16_t fetched = RYZ_SCAN_MAX_ENTRIES;
        esp_err_t records_error = esp_wifi_scan_get_ap_records(
            &fetched,
            s_scan_driver_records);
        if (records_error != ESP_OK) {
            error = records_error;
            (void)esp_wifi_clear_ap_list();
        } else {
            for (uint16_t index = 0; index < fetched; ++index) {
                const wifi_ap_record_t *source = &s_scan_driver_records[index];
                size_t length = strnlen(
                    (const char *)source->ssid,
                    sizeof(source->ssid));
                if (length == 0 || length > RYZ_PROVISIONING_SSID_MAX_LENGTH ||
                    source->primary < 1 || source->primary > 14) {
                    continue;
                }
                char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1] = {0};
                memcpy(ssid, source->ssid, length);
                if (scan_has_ssid(records, record_count, ssid) ||
                    record_count >= RYZ_SCAN_MAX_ENTRIES) {
                    continue;
                }
                copy_string(records[record_count].ssid,
                            sizeof(records[record_count].ssid), ssid);
                records[record_count].channel = source->primary;
                records[record_count].rssi = source->rssi;
                records[record_count].authmode = (uint8_t)source->authmode;
                if (record_count == 0 || source->rssi < rssi_min) rssi_min = source->rssi;
                if (record_count == 0 || source->rssi > rssi_max) rssi_max = source->rssi;
                ++record_count;
                if (!scan_has_channel(channels, channel_count, source->primary) &&
                    channel_count < RYZ_SCAN_MAX_CHANNELS) {
                    channels[channel_count++] = source->primary;
                }
            }
        }
    }
#else
    /* The host contract harness has no radio. It still links the real portal
     * adapter and exposes the deterministic failure so the UI can reveal its
     * manual-SSID fallback without inventing networks. */
    error = ESP_ERR_NOT_SUPPORTED;
#endif

    finish_scan(
        error,
        records,
        record_count,
        total_count,
        channels,
        channel_count,
        rssi_min,
        rssi_max);
}

static void process_scan_request(void)
{
    if (!take_scan_request()) return;
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    bool active = portal_session_active() && s_wifi_started;
    if (active) {
        portENTER_CRITICAL(&s_command_mux);
        active = s_network_mode == NETWORK_MODE_AP ||
                 s_network_mode == NETWORK_MODE_STOPPED;
        portEXIT_CRITICAL(&s_command_mux);
    }
    if (active) {
        perform_scan();
    } else {
        finish_scan(ESP_ERR_INVALID_STATE, NULL, 0, 0, NULL, 0, 0, 0);
    }
    xSemaphoreGive(s_network_mutex);
}

static bool scan_work_pending(void)
{
    portENTER_CRITICAL(&s_scan_mux);
    bool pending = s_scan_requested;
    portEXIT_CRITICAL(&s_scan_mux);
    return pending;
}

#if defined(ESP_PLATFORM)
static esp_err_t scan_post_handler(httpd_req_t *request)
{
    (void)request;
    bool accepting;
    portENTER_CRITICAL(&s_command_mux);
    accepting = s_portal_session &&
        (s_network_mode == NETWORK_MODE_AP || s_network_mode == NETWORK_MODE_STOPPED);
    portEXIT_CRITICAL(&s_command_mux);
    if (!accepting) {
        return send_json_error(request, "409 Conflict", "scan unavailable while connecting");
    }
    request_scan();
    set_common_headers(request);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cancel_post_handler(httpd_req_t *request)
{
    (void)request;
    if (!request_cancel()) {
        return send_json_error(
            request,
            "409 Conflict",
            "no active connection to cancel");
    }
    set_common_headers(request);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t redirect_404_handler(
    httpd_req_t *request,
    httpd_err_code_t error_code)
{
    (void)error_code;
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    set_common_headers(request);
    return httpd_resp_send(
        request,
        "Redirecting to the Ryzobee network portal",
        HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server_locked(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
#if defined(ESP_PLATFORM)
    config.max_uri_handlers = 9;
    config.uri_match_fn = httpd_uri_match_wildcard;
#else
    config.max_uri_handlers = 4;
#endif
    config.lru_purge_enable = true;
    esp_err_t error = httpd_start(&s_http_server, &config);
    if (error != ESP_OK) {
        s_http_server = NULL;
        return error;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t provision = {
        .uri = "/api/provision",
        .method = HTTP_POST,
        .handler = provision_post_handler,
    };
    const httpd_uri_t web_css = {
        .uri = "/assets/ryz-web.css", .method = HTTP_GET, .handler = web_css_get_handler,
    };
#if defined(ESP_PLATFORM)
    const httpd_uri_t diagnostic_page = {
        .uri = "/f/*", .method = HTTP_GET, .handler = diagnostics_page_get_handler,
    };
    const httpd_uri_t diagnostic_json = {
        .uri = "/api/diagnostics/*", .method = HTTP_GET, .handler = diagnostics_json_get_handler,
    };
    const httpd_uri_t scan = {
        .uri = "/api/scan",
        .method = HTTP_POST,
        .handler = scan_post_handler,
    };
    const httpd_uri_t cancel = {
        .uri = "/api/cancel",
        .method = HTTP_POST,
        .handler = cancel_post_handler,
    };
#endif
    error = httpd_register_uri_handler(s_http_server, &root);
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &status);
    }
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &provision);
    }
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &web_css);
    }
#if defined(ESP_PLATFORM)
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &diagnostic_page);
    }
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &diagnostic_json);
    }
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &scan);
    }
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &cancel);
    }
#endif
    if (error == ESP_OK) {
        error = httpd_register_err_handler(
            s_http_server,
            HTTPD_404_NOT_FOUND,
            redirect_404_handler);
    }
    if (error != ESP_OK) {
        if (httpd_stop(s_http_server) == ESP_OK) {
            s_http_server = NULL;
        }
    }
    return error;
}

static esp_err_t configure_captive_portal_dhcp_locked(
    ryz_provisioning_portal_info_t *out_portal_info)
{
    if (out_portal_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t error = esp_netif_get_ip_info(s_ap_netif, &ip_info);
    if (error != ESP_OK) {
        return error;
    }
    inet_ntoa_r(
        ip_info.ip.addr,
        out_portal_info->ipv4,
        sizeof(out_portal_info->ipv4));
    if (out_portal_info->ipv4[0] == '\0') {
        return ESP_FAIL;
    }
    snprintf(
        s_captive_url,
        sizeof(s_captive_url),
        "http://%s",
        out_portal_info->ipv4);

    esp_err_t stop_error = esp_netif_dhcps_stop(s_ap_netif);
    if (stop_error != ESP_OK &&
        stop_error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "could not pause DHCP server: %s", esp_err_to_name(stop_error));
        return ESP_OK;
    }
    esp_err_t option_error = esp_netif_dhcps_option(
        s_ap_netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_CAPTIVEPORTAL_URI,
        s_captive_url,
        strlen(s_captive_url));
    esp_err_t start_error = esp_netif_dhcps_start(s_ap_netif);
    if (start_error != ESP_OK &&
        start_error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(
            TAG,
            "DHCP server could not be restarted: %s",
            esp_err_to_name(start_error));
        return start_error;
    }
    if (option_error != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive URL could not be enabled");
    }
    return ESP_OK;
}

static esp_err_t start_portal_locked(
    ryz_provisioning_portal_info_t *out_portal_info)
{
    if (out_portal_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_portal_info, 0, sizeof(*out_portal_info));
    esp_err_t error = stop_network_locked();
    if (error != ESP_OK) {
        return error;
    }

    wifi_config_t wifi_config = {0};
    size_t ssid_length = strlen(s_ap_ssid);
    size_t password_length = strlen(s_ap_password);
    if (ssid_length == 0 || ssid_length > sizeof(wifi_config.ap.ssid) ||
        password_length < 8 || password_length > RYZ_PROVISIONING_PASSWORD_MAX_LENGTH) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(wifi_config.ap.ssid, s_ap_ssid, ssid_length);
    wifi_config.ap.ssid_len = (uint8_t)ssid_length;
    memcpy(wifi_config.ap.password, s_ap_password, password_length);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.capable = true;
    wifi_config.ap.pmf_cfg.required = false;

    error = esp_wifi_set_mode(RYZ_PORTAL_WIFI_MODE);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error != ESP_OK) {
        return error;
    }
    s_wifi_started = true;
    portENTER_CRITICAL(&s_command_mux);
    s_network_mode = NETWORK_MODE_AP;
    portEXIT_CRITICAL(&s_command_mux);
    error = configure_captive_portal_dhcp_locked(out_portal_info);
    if (error != ESP_OK) {
        stop_network_locked();
        return error;
    }

    error = start_http_server_locked();
    if (error == ESP_OK) {
        error = ryz_captive_dns_start(s_ap_netif, &s_dns_server);
    }
    if (error != ESP_OK) {
        stop_network_locked();
        return error;
    }
    portENTER_CRITICAL(&s_command_mux);
    copy_string(s_portal_ipv4, sizeof(s_portal_ipv4), out_portal_info->ipv4);
    s_portal_started_us = (uint64_t)esp_timer_get_time();
    s_portal_session = true;
    portEXIT_CRITICAL(&s_command_mux);
#if defined(ESP_PLATFORM)
    request_scan();
#endif
    ESP_LOGI(TAG, "provisioning portal ready on SSID %s", s_ap_ssid);
    return ESP_OK;
}

esp_err_t ryz_provisioning_platform_start_portal(
    ryz_provisioning_portal_info_t *out_portal_info)
{
    if (s_network_mutex == NULL || out_portal_info == NULL) {
        return s_network_mutex == NULL ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    esp_err_t error = start_portal_locked(out_portal_info);
    if (error != ESP_OK) {
        memset(out_portal_info, 0, sizeof(*out_portal_info));
    }
    xSemaphoreGive(s_network_mutex);
    return error;
}

static esp_err_t scan_saved_ssid_locked(const char *ssid)
{
    /* Directed scan avoids the portal cache's strongest-16 truncation. A
     * hidden AP may reply to the directed probe; an empty/different SSID is
     * never considered a match. This runs on the service owner, not the UI or
     * ESP event loop; the network mutex excludes the portal scan worker. */
    wifi_scan_config_t scan = {0};
    scan.ssid = (uint8_t *)ssid;
    scan.show_hidden = true;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.max = 100;
    scan.channel_bitmap.ghz_2_channels = 0x7FFE;
    scan.channel_bitmap.ghz_5_channels = 0;
    esp_err_t error = esp_wifi_scan_start(&scan, true);
    if (error != ESP_OK) {
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_clear_ap_list();
        return error;
    }
    wifi_ap_record_t record = {0};
    uint16_t count = 1;
    /* get_ap_records releases the whole driver list, not just this record.
     * The SSID filter is applied before truncation; only one match is needed. */
    error = esp_wifi_scan_get_ap_records(&count, &record);
    if (error != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return error;
    }
    const size_t length = strlen(ssid);
    return count && record.primary >= 1 && record.primary <= 14 &&
        strnlen((const char *)record.ssid, sizeof(record.ssid)) == length &&
        memcmp(record.ssid, ssid, length) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t start_sta_locked(const ryz_provisioning_credentials_t *credentials,
                                   bool scan_first)
{
    esp_err_t error = stop_network_locked();
    if (error != ESP_OK) {
        return error;
    }

    wifi_config_t wifi_config = {0};
    size_t ssid_length = strlen(credentials->ssid);
    size_t password_length = strlen(credentials->password);
    memcpy(wifi_config.sta.ssid, credentials->ssid, ssid_length);
    memcpy(wifi_config.sta.password, credentials->password, password_length);
    wifi_config.sta.threshold.authmode = password_length == 0
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    copy_string(s_sta_credentials.ssid, sizeof(s_sta_credentials.ssid), credentials->ssid);
    copy_string(s_sta_credentials.password, sizeof(s_sta_credentials.password), credentials->password);

    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error == ESP_OK) {
        s_wifi_started = true;
        /* Logical mode stays STOPPED during scan, so delayed disconnect/IP
         * events cannot create reconnect work before this admission check. */
        if (scan_first) error = scan_saved_ssid_locked(credentials->ssid);
    }
    if (error == ESP_OK) {
        if (scan_first)
            emit_event(RYZ_PROVISIONING_PLATFORM_CONNECTING, ESP_OK, 0, credentials->ssid, NULL);
        portENTER_CRITICAL(&s_command_mux);
        s_network_mode = NETWORK_MODE_STA;
        s_connect_started_us = (uint64_t)esp_timer_get_time();
        portEXIT_CRITICAL(&s_command_mux);
        error = esp_wifi_connect();
    }
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_command_mux);
        s_connect_started_us = 0;
        portEXIT_CRITICAL(&s_command_mux);
        stop_network_locked();
    }
    return error;
}

#if defined(ESP_PLATFORM)
/* A browser connected to the SoftAP must remain able to poll the four Figma
 * states. APSTA keeps the HTTP/DNS services alive while the STA joins the
 * selected router; the logical mode still becomes STA for the existing
 * generation/retry machinery. */
static esp_err_t start_sta_keep_portal_locked(
    const ryz_provisioning_credentials_t *credentials)
{
    if (!s_wifi_started || !s_portal_session || credentials == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_config = {0};
    size_t ssid_length = strlen(credentials->ssid);
    size_t password_length = strlen(credentials->password);
    memcpy(wifi_config.sta.ssid, credentials->ssid, ssid_length);
    memcpy(wifi_config.sta.password, credentials->password, password_length);
    wifi_config.sta.threshold.authmode = password_length == 0
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (error != ESP_OK) return error;
    copy_string(s_sta_credentials.ssid, sizeof(s_sta_credentials.ssid), credentials->ssid);
    copy_string(s_sta_credentials.password, sizeof(s_sta_credentials.password), credentials->password);
    portENTER_CRITICAL(&s_command_mux);
    s_network_mode = NETWORK_MODE_STA;
    s_connect_started_us = (uint64_t)esp_timer_get_time();
    portEXIT_CRITICAL(&s_command_mux);
    error = esp_wifi_connect();
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_command_mux);
        s_network_mode = NETWORK_MODE_STOPPED;
        s_connect_started_us = 0;
        portEXIT_CRITICAL(&s_command_mux);
    }
    return error;
}
#endif

esp_err_t ryz_provisioning_platform_start_sta(
    const ryz_provisioning_credentials_t *credentials)
{
    if (s_network_mutex == NULL || !is_valid_credentials(credentials)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    esp_err_t error = start_sta_locked(credentials, false);
    xSemaphoreGive(s_network_mutex);
    return error;
}

esp_err_t ryz_provisioning_platform_start_saved_sta(
    const ryz_provisioning_credentials_t *credentials)
{
    if (s_network_mutex == NULL || !is_valid_credentials(credentials))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    esp_err_t error = start_sta_locked(credentials, true);
    xSemaphoreGive(s_network_mutex);
    return error;
}

static void process_portal_credentials(const connect_command_t *command)
{
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_command_mux);
    ryz_provisioning_generation_state_t state = command_state_locked();
    bool portal_mode = s_network_mode == NETWORK_MODE_AP;
#if defined(ESP_PLATFORM)
    portal_mode = portal_mode ||
        (s_portal_session && s_network_mode == NETWORK_MODE_STOPPED);
#endif
    bool current = portal_mode &&
                   ryz_provisioning_claim_connect_command(
                       &state, command->generation);
    bool keep_portal = current && s_portal_session;
    if (current) {
        store_command_state_locked(&state);
#if !defined(ESP_PLATFORM)
        s_network_mode = NETWORK_MODE_STOPPED;
#else
        if (!keep_portal) s_network_mode = NETWORK_MODE_STOPPED;
#endif
    }
    portEXIT_CRITICAL(&s_command_mux);
    if (current) {
        emit_event(
            RYZ_PROVISIONING_PLATFORM_CONNECTING,
            ESP_OK,
            0,
            command->credentials.ssid,
            NULL);
        esp_err_t error;
#if defined(ESP_PLATFORM)
        error = keep_portal
            ? start_sta_keep_portal_locked(&command->credentials)
            : start_sta_locked(&command->credentials, false);
#else
        (void)keep_portal;
        error = start_sta_locked(&command->credentials, false);
#endif
        if (error != ESP_OK) {
            portENTER_CRITICAL(&s_command_mux);
            s_connect_started_us = 0;
            portEXIT_CRITICAL(&s_command_mux);
            emit_event(
                RYZ_PROVISIONING_PLATFORM_FAILED,
                error,
                0,
                command->credentials.ssid,
                NULL);
        }
    }
    xSemaphoreGive(s_network_mutex);
}

/* The ESP event loop delivers asynchronously and its Wi-Fi payloads have no
 * application epoch. The generation protects our own copied commands, not
 * event provenance. ONLINE therefore requires a live associated AP and a
 * live (not esp-netif's down-interface cached) IP at observation time. */
static bool sta_is_associated_locked(void)
{
    wifi_ap_record_t access_point = {0};
    size_t ssid_length = strlen(s_sta_credentials.ssid);
    return s_wifi_started && ssid_length > 0 &&
        esp_wifi_sta_get_ap_info(&access_point) == ESP_OK &&
        strnlen((const char *)access_point.ssid, sizeof(access_point.ssid)) ==
            ssid_length &&
        memcmp(access_point.ssid, s_sta_credentials.ssid, ssid_length) == 0;
}

static bool read_live_sta_ip_locked(char *ipv4, size_t ipv4_size)
{
    esp_netif_ip_info_t ip_info = {0};
    if (s_sta_netif == NULL || !sta_is_associated_locked() ||
        !esp_netif_is_netif_up(s_sta_netif) ||
        esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        return false;
    }
    inet_ntoa_r(ip_info.ip.addr, ipv4, ipv4_size);
    return ipv4[0] != '\0';
}

static void publish_online_locked(const char *ipv4)
{
    if (!s_link_binding.valid || s_link_binding.generation != s_generation ||
        strcmp(s_link_binding.ssid, s_sta_credentials.ssid) != 0 ||
        strcmp(s_link_binding.ipv4, ipv4) != 0) {
        revoke_local_secret_locked();
        ryz_net_traffic_revoke();
    }
    s_link_binding.valid = true;
    s_link_binding.generation = s_generation;
    copy_string(s_link_binding.ssid, sizeof(s_link_binding.ssid), s_sta_credentials.ssid);
    copy_string(s_link_binding.ipv4, sizeof(s_link_binding.ipv4), ipv4);
    s_link_binding.next_sample_us = 0;
    portENTER_CRITICAL(&s_command_mux);
    s_connect_started_us = 0;
    portEXIT_CRITICAL(&s_command_mux);
    emit_event(RYZ_PROVISIONING_PLATFORM_ONLINE, ESP_OK, 0, s_sta_credentials.ssid, ipv4);
}

/* Unlike a best-effort diagnostics getter, a failed observation must not be
 * interpreted as permission to disconnect a possibly live saved station. */
static esp_err_t boot_live_sta_ip_locked(char *ipv4, size_t size)
{
    ipv4[0] = '\0';
    if (!s_wifi_started || s_network_mode == NETWORK_MODE_STOPPED) return ESP_OK;
    if (s_network_mode != NETWORK_MODE_STA || s_sta_netif == NULL ||
        !s_sta_credentials.ssid[0]) return ESP_ERR_INVALID_STATE;
    wifi_ap_record_t before = {0}, after = {0};
    esp_err_t error = esp_wifi_sta_get_ap_info(&before);
    if (error == ESP_ERR_WIFI_NOT_CONNECT || error == ESP_ERR_WIFI_NOT_STARTED)
        return ESP_OK;
    if (error != ESP_OK) return error;
    const size_t length = strlen(s_sta_credentials.ssid);
    if (strnlen((const char *)before.ssid, sizeof(before.ssid)) != length ||
        memcmp(before.ssid, s_sta_credentials.ssid, length) != 0 ||
        !esp_netif_is_netif_up(s_sta_netif)) return ESP_OK;
    esp_netif_ip_info_t ip = {0};
    error = esp_netif_get_ip_info(s_sta_netif, &ip);
    if (error != ESP_OK) return error;
    if (!ip.ip.addr) return ESP_OK;
    error = esp_wifi_sta_get_ap_info(&after);
    if (error == ESP_ERR_WIFI_NOT_CONNECT || error == ESP_ERR_WIFI_NOT_STARTED)
        return ESP_OK;
    if (error != ESP_OK) return error;
    if (!esp_netif_is_netif_up(s_sta_netif)) return ESP_OK;
    /* A roam during the read makes the IP/association pairing uncertain, not
     * either a verified ONLINE connection or a confirmed failed connection. */
    if (memcmp(before.bssid, after.bssid, sizeof(before.bssid)) != 0 ||
        strnlen((const char *)after.ssid, sizeof(after.ssid)) != length ||
        memcmp(after.ssid, s_sta_credentials.ssid, length) != 0)
        return ESP_ERR_INVALID_STATE;
    inet_ntoa_r(ip.ip.addr, ipv4, size);
    return ipv4[0] ? ESP_OK : ESP_FAIL;
}

esp_err_t ryz_provisioning_platform_boot_fallback(void)
{
    if (s_network_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    char ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1] = {0};
    /* No adoption of an HTTP-submitted or manually opened portal session. */
    esp_err_t error = s_portal_session || s_network_mode == NETWORK_MODE_AP
        ? ESP_ERR_INVALID_STATE : boot_live_sta_ip_locked(ipv4, sizeof(ipv4));
    if (error == ESP_OK && ipv4[0]) {
        portENTER_CRITICAL(&s_command_mux);
        ryz_provisioning_generation_state_t state = command_state_locked();
        ryz_provisioning_mark_online(&state);
        store_command_state_locked(&state);
        portEXIT_CRITICAL(&s_command_mux);
        publish_online_locked(ipv4);
    } else if (error == ESP_OK) {
        emit_event(RYZ_PROVISIONING_PLATFORM_BOOT_AP_STARTING, ESP_OK, 0, NULL, NULL);
        ryz_provisioning_portal_info_t portal = {0};
        /* This helper first revokes the entire old event/retry generation and
         * confirms shutdown, before starting WPA2 AP + HTTP + captive DNS. */
        error = start_portal_locked(&portal);
        if (error == ESP_OK && !portal.ipv4[0]) error = ESP_FAIL;
        if (error == ESP_OK)
            emit_event(RYZ_PROVISIONING_PLATFORM_PORTAL_READY, ESP_OK, 0, NULL, portal.ipv4);
    }
    if (error != ESP_OK)
        emit_event(RYZ_PROVISIONING_PLATFORM_FAILED, error, 0, NULL, NULL);
    xSemaphoreGive(s_network_mutex);
    return error;
}

static esp_err_t read_link_identity_locked(wifi_ap_record_t *access_point)
{
    if (!s_wifi_started || s_sta_netif == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t error = esp_wifi_sta_get_ap_info(access_point);
    if (error != ESP_OK) return error;
    size_t length = strlen(s_link_binding.ssid);
    if (!length || strnlen((const char *)access_point->ssid, sizeof(access_point->ssid)) != length ||
        memcmp(access_point->ssid, s_link_binding.ssid, length) != 0 ||
        !esp_netif_is_netif_up(s_sta_netif)) return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t ip_info = {0};
    error = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (error != ESP_OK) return error;
    char ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1] = {0};
    if (ip_info.ip.addr == 0 || !inet_ntoa_r(ip_info.ip.addr, ipv4, sizeof(ipv4)) ||
        strcmp(ipv4, s_link_binding.ipv4) != 0) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t read_link_info_locked(ryz_provisioning_link_info_t *out, uint8_t bssid[6])
{
    wifi_ap_record_t before = {0}, after = {0};
    esp_err_t error = read_link_identity_locked(&before);
    if (error != ESP_OK) return error;
    uint8_t mac[6] = {0};
    error = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (error != ESP_OK) return error;
    snprintf(out->mac, sizeof(out->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    out->mac_valid = true;
    out->rssi_valid = true;
    out->rssi_dbm = before.rssi;
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t address = {0};
    error = esp_netif_get_ip6_global(s_sta_netif, &address);
    if (error == ESP_FAIL) error = esp_netif_get_ip6_linklocal(s_sta_netif, &address);
    if (error == ESP_OK) {
        if (!(address.addr[0] | address.addr[1] | address.addr[2] | address.addr[3]) ||
            !inet6_ntoa_r(address, out->ipv6, sizeof(out->ipv6))) return ESP_ERR_INVALID_STATE;
        out->ipv6_valid = true;
    } else if (error != ESP_FAIL) {
        return error; /* No preferred address (ESP_FAIL) is an ordinary absence. */
    }
#endif
    /* SDK radio transitions are asynchronous even while our control mutex is
     * held. Re-observe association/IP so a mid-sample disconnect or roam does
     * not combine an old RSSI with a new connection's address. */
    error = read_link_identity_locked(&after);
    if (error == ESP_OK && memcmp(before.bssid, after.bssid, sizeof(before.bssid)) != 0)
        error = ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) memcpy(bssid, after.bssid, sizeof(after.bssid));
    return error;
}

static TickType_t sample_link_info(void)
{
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    if (!s_link_binding.valid || s_network_mode != NETWORK_MODE_STA ||
        s_link_binding.generation != s_generation ||
        strcmp(s_link_binding.ssid, s_sta_credentials.ssid) != 0) {
        xSemaphoreGive(s_network_mutex);
        return portMAX_DELAY;
    }
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (now >= s_link_binding.next_sample_us) {
        /* Consume only the revocation known before this entire live sample.
         * A disconnect during the SDK getters stays pending and cannot grant
         * an epoch. No event callback takes this network mutex. */
        portENTER_CRITICAL(&s_command_mux);
        bool revoke = s_secret_revoke_pending;
        s_secret_revoke_pending = false;
        if (revoke) ryz_net_traffic_revoke();
        portEXIT_CRITICAL(&s_command_mux);
        if (revoke) revoke_local_secret_locked();
        ryz_provisioning_platform_event_t event = {.type = RYZ_PROVISIONING_PLATFORM_LINK_INFO};
        copy_string(event.ssid, sizeof(event.ssid), s_link_binding.ssid);
        copy_string(event.ipv4, sizeof(event.ipv4), s_link_binding.ipv4);
        uint8_t bssid[6] = {0};
        esp_err_t error = read_link_info_locked(&event.link, bssid);
        if (error != ESP_OK) {
            memset(&event.link, 0, sizeof(event.link));
            revoke_local_secret_locked();
            ryz_net_traffic_revoke();
        }
        event.link.last_error = error;
        if (error == ESP_OK) {
            ryz_net_traffic_snapshot_t traffic = {0};
            /* Serialize binding with the raw disconnect revocation, not with
             * queued event delivery. Lock order is command -> traffic; packet
             * wrappers never acquire command/network locks or call SDK while
             * holding the traffic lock. A failed counter remains independent
             * of the successfully sampled RSSI/MAC/IP fields. */
            portENTER_CRITICAL(&s_command_mux);
            if (!s_secret_revoke_pending &&
                ryz_net_traffic_observe(s_sta_netif, bssid, &traffic) == ESP_OK &&
                traffic.valid) {
                event.link.traffic_valid = true;
                event.link.traffic_epoch = traffic.epoch;
                event.link.traffic_rx_bytes = traffic.rx_bytes;
                event.link.traffic_tx_bytes = traffic.tx_bytes;
            }
            portEXIT_CRITICAL(&s_command_mux);
        }
        event.link.sampled_at_us = (uint64_t)esp_timer_get_time();
        if (error == ESP_OK) observe_local_secret_locked(bssid, event.link.sampled_at_us);
        s_link_binding.next_sample_us = event.link.sampled_at_us + RYZ_LINK_SAMPLE_INTERVAL_US;
        if (s_event_sink != NULL) s_event_sink(&event);
        now = event.link.sampled_at_us;
    }
    uint64_t milliseconds = (s_link_binding.next_sample_us - now + 999U) / 1000U;
    TickType_t wait_ticks = pdMS_TO_TICKS(milliseconds);
    if (!wait_ticks) wait_ticks = 1;
    xSemaphoreGive(s_network_mutex);
    return wait_ticks;
}

static bool process_sta_got_ip(const network_event_command_t *command)
{
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_command_mux);
    ryz_provisioning_generation_state_t state = command_state_locked();
    bool current = ryz_provisioning_network_event_is_current(
        &state,
        s_network_mode == NETWORK_MODE_STA,
        command->generation);
    portEXIT_CRITICAL(&s_command_mux);
    char live_ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1] = {0};
    current = current && read_live_sta_ip_locked(live_ipv4, sizeof(live_ipv4)) &&
              strcmp(command->ipv4, live_ipv4) == 0;
    if (current) {
        portENTER_CRITICAL(&s_command_mux);
        ryz_provisioning_mark_online(&state);
        store_command_state_locked(&state);
        portEXIT_CRITICAL(&s_command_mux);
    }
    if (current) {
        publish_online_locked(command->ipv4);
    }
    xSemaphoreGive(s_network_mutex);
    return current;
}

static ryz_provisioning_reconnect_action_t process_sta_disconnected(
    const network_event_command_t *command,
    bool observed_online,
    ryz_provisioning_reconnect_ticket_t *out_ticket)
{
    ryz_provisioning_reconnect_action_t action =
        RYZ_PROVISIONING_RECONNECT_IGNORE;

    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    wifi_ap_record_t access_point = {0};
    /* A queued disconnect for a different SSID, or one delivered after the
     * radio is already associated again, must not tear down that connection.
     * Same-SSID disconnected payloads still have no SDK epoch; their reason
     * remains an observation, not proof of a particular connection attempt. */
    if (strcmp(command->ssid, s_sta_credentials.ssid) != 0 ||
        esp_wifi_sta_get_ap_info(&access_point) != ESP_ERR_WIFI_NOT_CONNECT) {
        xSemaphoreGive(s_network_mutex);
        return action;
    }
    portENTER_CRITICAL(&s_command_mux);
    ryz_provisioning_generation_state_t state = command_state_locked();
    if (ryz_provisioning_network_event_is_current(
            &state,
            s_network_mode == NETWORK_MODE_STA,
            command->generation)) {
        action = ryz_provisioning_handle_disconnect(
            &state,
            s_network_mode == NETWORK_MODE_STA,
            observed_online,
            disconnect_is_credentials_failure(command->disconnect_reason),
            out_ticket);
        store_command_state_locked(&state);
        if (action == RYZ_PROVISIONING_RECONNECT_FAILED_CREDENTIALS ||
            action == RYZ_PROVISIONING_RECONNECT_FAILED_EXHAUSTED) {
            s_network_mode = NETWORK_MODE_STOPPED;
        }
    }
    portEXIT_CRITICAL(&s_command_mux);

    if (action != RYZ_PROVISIONING_RECONNECT_IGNORE) {
        s_link_binding.valid = false;
        revoke_local_secret_locked();
        ryz_net_traffic_revoke();
    }
    if (action == RYZ_PROVISIONING_RECONNECT_RETRY) {
        emit_event(
            RYZ_PROVISIONING_PLATFORM_CONNECTING,
            ESP_OK,
            0,
            s_sta_credentials.ssid,
            NULL);
    } else if (action == RYZ_PROVISIONING_RECONNECT_FAILED_CREDENTIALS ||
               action == RYZ_PROVISIONING_RECONNECT_FAILED_EXHAUSTED) {
        portENTER_CRITICAL(&s_command_mux);
        s_connect_started_us = 0;
        portEXIT_CRITICAL(&s_command_mux);
        emit_event(
            RYZ_PROVISIONING_PLATFORM_FAILED,
            ESP_FAIL,
            command->disconnect_reason,
            s_sta_credentials.ssid,
            NULL);
    }
    xSemaphoreGive(s_network_mutex);
    return action;
}

static void process_reconnect(
    const ryz_provisioning_reconnect_ticket_t *ticket,
    uint8_t disconnect_reason)
{
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_command_mux);
    ryz_provisioning_generation_state_t state = command_state_locked();
    bool current = s_network_mode == NETWORK_MODE_STA &&
        state.reconnect_pending && ticket->generation == state.generation &&
        ticket->attempt == state.reconnect_attempts;
    portEXIT_CRITICAL(&s_command_mux);
    char live_ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1] = {0};
    bool online = current && read_live_sta_ip_locked(live_ipv4, sizeof(live_ipv4));
    portENTER_CRITICAL(&s_command_mux);
    if (online) {
        ryz_provisioning_mark_online(&state);
        store_command_state_locked(&state);
        current = false;
    } else if (current) {
        current = ryz_provisioning_claim_reconnect(
            &state, s_network_mode == NETWORK_MODE_STA, ticket);
        if (current) {
            store_command_state_locked(&state);
        }
    }
    portEXIT_CRITICAL(&s_command_mux);
    if (online) {
        publish_online_locked(live_ipv4);
    }
    if (current) {
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            bool associated = sta_is_associated_locked();
            online = associated &&
                read_live_sta_ip_locked(live_ipv4, sizeof(live_ipv4));
            portENTER_CRITICAL(&s_command_mux);
            if (online) {
                state = command_state_locked();
                ryz_provisioning_mark_online(&state);
                store_command_state_locked(&state);
            } else if (!associated) {
                begin_network_transition_locked();
            }
            portEXIT_CRITICAL(&s_command_mux);
            if (online) {
                publish_online_locked(live_ipv4);
            } else if (!associated) {
                emit_event(
                    RYZ_PROVISIONING_PLATFORM_FAILED,
                    error,
                    disconnect_reason,
                    s_sta_credentials.ssid,
                    NULL);
            }
        }
    }
    xSemaphoreGive(s_network_mutex);
}

#if defined(ESP_PLATFORM)
static void process_cancel_request(void)
{
    uint32_t requested_generation = 0;
    if (!take_cancel_request(&requested_generation)) return;

    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    char portal_ip[sizeof(s_portal_ipv4)] = {0};
    bool current = false;
    portENTER_CRITICAL(&s_command_mux);
    current = s_portal_session && s_http_server != NULL &&
        requested_generation == s_generation &&
        (s_connect_pending || s_network_mode == NETWORK_MODE_STA);
    if (current) {
        copy_string(portal_ip, sizeof(portal_ip), s_portal_ipv4);
        /* Invalidate queued commands, reconnect tickets, and raw SDK events
         * before disconnecting. The callback that follows therefore observes
         * AP mode and cannot publish a late station result. */
        begin_network_transition_locked();
        s_network_mode = NETWORK_MODE_AP;
        s_connect_started_us = 0;
    }
    portEXIT_CRITICAL(&s_command_mux);

    if (current) {
        esp_err_t disconnect_error = esp_wifi_disconnect();
        if (disconnect_error != ESP_OK &&
            disconnect_error != ESP_ERR_WIFI_NOT_CONNECT &&
            disconnect_error != ESP_ERR_WIFI_NOT_STARTED) {
            /* APSTA should still be usable. A best-effort mode fallback keeps
             * the STA side from reconnecting if the driver rejected the
             * asynchronous disconnect request. */
            ESP_LOGW(TAG, "cancel disconnect failed: %s",
                     esp_err_to_name(disconnect_error));
            (void)esp_wifi_set_mode(WIFI_MODE_AP);
        }
        wipe_secret(&s_sta_credentials, sizeof(s_sta_credentials));
        memset(&s_link_binding, 0, sizeof(s_link_binding));
        emit_event(
            RYZ_PROVISIONING_PLATFORM_PORTAL_READY,
            ESP_OK,
            0,
            NULL,
            portal_ip);
        request_scan();
    }
    xSemaphoreGive(s_network_mutex);
}

static void process_connect_timeout(void)
{
    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    uint64_t started = 0;
    bool current = false;
    portENTER_CRITICAL(&s_command_mux);
    started = s_connect_started_us;
    current = s_portal_session && s_http_server != NULL &&
        s_network_mode == NETWORK_MODE_STA && started != 0;
    portEXIT_CRITICAL(&s_command_mux);
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (current && now >= started && now - started >= RYZ_CONNECT_TIMEOUT_US) {
        char ssid[sizeof(s_sta_credentials.ssid)] = {0};
        copy_string(ssid, sizeof(ssid), s_sta_credentials.ssid);
        portENTER_CRITICAL(&s_command_mux);
        begin_network_transition_locked();
        s_connect_started_us = 0;
        s_network_mode = NETWORK_MODE_STOPPED;
        portEXIT_CRITICAL(&s_command_mux);
        (void)esp_wifi_disconnect();
        wipe_secret(&s_sta_credentials, sizeof(s_sta_credentials));
        memset(&s_link_binding, 0, sizeof(s_link_binding));
        emit_event(
            RYZ_PROVISIONING_PLATFORM_FAILED,
            ESP_ERR_TIMEOUT,
            0,
            ssid,
            NULL);
    }
    xSemaphoreGive(s_network_mutex);
}

static TickType_t connect_timeout_wait_ticks(void)
{
    uint64_t started = 0;
    bool current = false;
    portENTER_CRITICAL(&s_command_mux);
    started = s_connect_started_us;
    current = s_portal_session && s_http_server != NULL &&
        s_network_mode == NETWORK_MODE_STA && started != 0;
    portEXIT_CRITICAL(&s_command_mux);
    if (!current) return portMAX_DELAY;
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (now >= started && now - started >= RYZ_CONNECT_TIMEOUT_US) return 0;
    uint64_t elapsed = now >= started ? now - started : 0;
    uint64_t remaining_us = RYZ_CONNECT_TIMEOUT_US - elapsed;
    uint64_t remaining_ms = (remaining_us + UINT64_C(999)) / UINT64_C(1000);
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)remaining_ms);
    return ticks == 0 ? 1 : ticks;
}
#endif

static void connect_worker(void *argument)
{
    (void)argument;
    connect_command_t portal_command = {0};
    bool portal_scheduled = false;
    ryz_provisioning_reconnect_ticket_t reconnect_ticket = {0};
    TickType_t reconnect_due_tick = 0;
    uint8_t reconnect_reason = 0;
    bool reconnect_scheduled = false;

    while (true) {
        connect_command_t queued_command;
        while (xQueueReceive(s_connect_queue, &queued_command, 0) == pdTRUE) {
            portal_command = queued_command;
            portal_scheduled = true;
            memset(&queued_command, 0, sizeof(queued_command));
        }

#if defined(ESP_PLATFORM)
        /* Cancellation and deadline handling run before queued commands or
         * SDK events. Both paths invalidate the generation first, so stale
         * local copies below are discarded in the same loop. */
        process_cancel_request();
        process_connect_timeout();
#endif

        network_event_command_t network_event = {0};
        portENTER_CRITICAL(&s_command_mux);
        bool portal_mode = s_network_mode == NETWORK_MODE_AP;
#if defined(ESP_PLATFORM)
        portal_mode = portal_mode ||
            (s_portal_session && s_network_mode == NETWORK_MODE_STOPPED);
#endif
        bool portal_current = portal_mode && s_connect_pending &&
            portal_command.generation == s_generation;
        bool reconnect_current = s_network_mode == NETWORK_MODE_STA &&
            s_reconnect_pending && reconnect_ticket.generation == s_generation;
        bool network_event_pending = s_network_event_pending;
        bool batch_saw_got_ip = s_network_event_saw_got_ip;
        uint32_t got_ip_generation = s_network_event_got_ip_generation;
        if (network_event_pending) {
            network_event = s_network_event_mailbox;
            s_network_event_pending = false;
            s_network_event_saw_got_ip = false;
        }
        portEXIT_CRITICAL(&s_command_mux);
        /* Stop wakes this worker even without a new HTTP request. Discard
         * revoked local copies now, rather than retaining credentials until
         * an obsolete timer expires. Commands already claimed must still
         * pass their network-mutex generation check before touching Wi-Fi. */
        if (portal_scheduled && !portal_current) {
            portal_scheduled = false;
            memset(&portal_command, 0, sizeof(portal_command));
        }
        if (reconnect_scheduled && !reconnect_current) {
            reconnect_scheduled = false;
        }

        if (network_event_pending) {
            if (network_event.type == NETWORK_EVENT_STA_GOT_IP) {
                if (process_sta_got_ip(&network_event)) {
                    reconnect_scheduled = false;
                }
            } else if (network_event.type ==
                       NETWORK_EVENT_STA_DISCONNECTED) {
                bool observed_online =
                    batch_saw_got_ip &&
                    got_ip_generation == network_event.generation;
                /* Only a validated processing result may replace or consume
                 * an existing ticket. Raw coalesced IP data followed by an
                 * ignored foreign/stale disconnect must leave it pending. */
                ryz_provisioning_reconnect_ticket_t ticket = {0};
                ryz_provisioning_reconnect_action_t action =
                    process_sta_disconnected(
                        &network_event,
                        observed_online,
                        &ticket);
                if (action == RYZ_PROVISIONING_RECONNECT_RETRY) {
                    reconnect_ticket = ticket;
                    reconnect_due_tick =
                        xTaskGetTickCount() + pdMS_TO_TICKS(ticket.delay_ms);
                    reconnect_reason = network_event.disconnect_reason;
                    reconnect_scheduled = true;
                } else if (
                    action == RYZ_PROVISIONING_RECONNECT_FAILED_CREDENTIALS ||
                    action == RYZ_PROVISIONING_RECONNECT_FAILED_EXHAUSTED) {
                    reconnect_scheduled = false;
                }
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (portal_scheduled &&
            (int32_t)(now - portal_command.due_tick) >= 0) {
            process_portal_credentials(&portal_command);
            portal_scheduled = false;
            memset(&portal_command, 0, sizeof(portal_command));
            now = xTaskGetTickCount();
        }
        if (reconnect_scheduled &&
            (int32_t)(now - reconnect_due_tick) >= 0) {
            process_reconnect(&reconnect_ticket, reconnect_reason);
            reconnect_scheduled = false;
            now = xTaskGetTickCount();
        }

        /* Scans are serialized in this worker and never run from the HTTP
         * task. A pending request wakes the loop immediately. */
        process_scan_request();
        now = xTaskGetTickCount();

        TickType_t wait_ticks = sample_link_info();
#if defined(ESP_PLATFORM)
        TickType_t timeout_wait = connect_timeout_wait_ticks();
        if (wait_ticks == portMAX_DELAY || timeout_wait < wait_ticks) {
            wait_ticks = timeout_wait;
        }
#endif
        if (portal_scheduled) {
            int32_t remaining = (int32_t)(portal_command.due_tick - now);
            TickType_t portal_wait = remaining <= 0 ? 0 : (TickType_t)remaining;
            if (wait_ticks == portMAX_DELAY || portal_wait < wait_ticks) wait_ticks = portal_wait;
        }
        if (reconnect_scheduled) {
            int32_t remaining = (int32_t)(reconnect_due_tick - now);
            TickType_t reconnect_wait =
                remaining <= 0 ? 0 : (TickType_t)remaining;
            if (wait_ticks == portMAX_DELAY || reconnect_wait < wait_ticks) {
                wait_ticks = reconnect_wait;
            }
        }
        if (scan_work_pending()) wait_ticks = 0;
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

static void network_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)argument;
    network_event_command_t command = {0};
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP &&
        event_data != NULL) {
        const ip_event_got_ip_t *event = event_data;
        if (event->esp_netif != s_sta_netif || event->ip_info.ip.addr == 0) {
            return;
        }
        command.type = NETWORK_EVENT_STA_GOT_IP;
        snprintf(
            command.ipv4,
            sizeof(command.ipv4),
            IPSTR,
            IP2STR(&event->ip_info.ip));
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        if (event == NULL || event->ssid_len == 0 ||
            event->ssid_len > RYZ_PROVISIONING_SSID_MAX_LENGTH) {
            return;
        }
        command.type = NETWORK_EVENT_STA_DISCONNECTED;
        memcpy(command.ssid, event->ssid, event->ssid_len);
        command.disconnect_reason = event->reason;
    } else {
        return;
    }

    portENTER_CRITICAL(&s_command_mux);
    bool current_sta = s_network_mode == NETWORK_MODE_STA;
    command.generation = s_generation;
    if (current_sta) {
        if (command.type == NETWORK_EVENT_STA_DISCONNECTED) {
            s_secret_revoke_pending = true;
            ryz_net_traffic_revoke();
        }
        s_network_event_mailbox = command;
        s_network_event_pending = true;
        if (command.type == NETWORK_EVENT_STA_GOT_IP) {
            s_network_event_saw_got_ip = true;
            s_network_event_got_ip_generation = command.generation;
        }
    }
    portEXIT_CRITICAL(&s_command_mux);
    if (current_sta) {
        xTaskNotifyGive(s_connect_task);
    }
}

static esp_err_t create_default_wifi_netif(
    bool access_point,
    esp_netif_t **out_netif)
{
    if (out_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_netif = NULL;

    esp_netif_config_t config;
    if (access_point) {
        config = (esp_netif_config_t)ESP_NETIF_DEFAULT_WIFI_AP();
    } else {
        config = (esp_netif_config_t)ESP_NETIF_DEFAULT_WIFI_STA();
    }
    esp_netif_t *netif = esp_netif_new(&config);
    if (netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = access_point
        ? esp_netif_attach_wifi_ap(netif)
        : esp_netif_attach_wifi_station(netif);
    if (error == ESP_OK) {
        error = access_point
            ? esp_wifi_set_default_wifi_ap_handlers()
            : esp_wifi_set_default_wifi_sta_handlers();
    }
    if (error != ESP_OK) {
        esp_wifi_clear_default_wifi_driver_and_handlers(netif);
        esp_netif_destroy(netif);
        return error;
    }

    *out_netif = netif;
    return ESP_OK;
}

static void cleanup_failed_init(ryz_provisioning_init_plan_t *plan)
{
    /* Unbind before any driver/netif can be destroyed or its address reused. */
    ryz_net_traffic_revoke();
    ryz_provisioning_init_resource_t resources[
        RYZ_PROVISIONING_INIT_RESOURCE_COUNT];
    size_t count = ryz_provisioning_init_take_rollback_plan(
        plan,
        resources,
        RYZ_PROVISIONING_INIT_RESOURCE_COUNT);
    for (size_t index = 0; index < count; ++index) {
        switch (resources[index]) {
        case RYZ_PROVISIONING_INIT_IP_HANDLER:
            esp_event_handler_instance_unregister(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                s_ip_event_instance);
            s_ip_event_instance = NULL;
            break;
        case RYZ_PROVISIONING_INIT_WIFI_HANDLER:
            esp_event_handler_instance_unregister(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                s_wifi_event_instance);
            s_wifi_event_instance = NULL;
            break;
        case RYZ_PROVISIONING_INIT_CONNECT_TASK:
            vTaskDelete(s_connect_task);
            s_connect_task = NULL;
            break;
        case RYZ_PROVISIONING_INIT_CONNECT_QUEUE:
            vQueueDelete(s_connect_queue);
            s_connect_queue = NULL;
            break;
        case RYZ_PROVISIONING_INIT_NETWORK_MUTEX:
            vSemaphoreDelete(s_network_mutex);
            s_network_mutex = NULL;
            break;
        case RYZ_PROVISIONING_INIT_WIFI:
            esp_wifi_deinit();
            s_wifi_started = false;
            break;
        case RYZ_PROVISIONING_INIT_STA_NETIF:
            esp_netif_destroy_default_wifi(s_sta_netif);
            s_sta_netif = NULL;
            break;
        case RYZ_PROVISIONING_INIT_AP_NETIF:
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
            break;
        case RYZ_PROVISIONING_INIT_EVENT_LOOP:
            esp_event_loop_delete_default();
            break;
        default:
            break;
        }
    }

    portENTER_CRITICAL(&s_command_mux);
    s_network_mode = NETWORK_MODE_STOPPED;
    s_generation = 0;
    s_connect_pending = false;
    s_cancel_requested = false;
    s_cancel_generation = 0;
    s_reconnect_attempts = 0;
    s_reconnect_pending = false;
    s_network_event_pending = false;
    s_network_event_saw_got_ip = false;
    portEXIT_CRITICAL(&s_command_mux);
    s_event_sink = NULL;
    memset(&s_link_binding, 0, sizeof(s_link_binding));
    s_portal_session = false;
    s_portal_ipv4[0] = '\0';
    s_connect_started_us = 0;
    s_portal_started_us = 0;
    reset_scan_cache();
    revoke_local_secret_locked();
    wipe_secret(&s_sta_credentials, sizeof(s_sta_credentials));
}

void ryz_provisioning_platform_deinit(void)
{
    /* This seam is used only to roll back the narrow gap between a committed
     * platform init and the core publishing s_initialized. Wi-Fi and portal
     * services have not been started at that point. */
    s_event_sink = NULL;
    cleanup_failed_init(&s_init_plan);
}

esp_err_t ryz_provisioning_platform_init(
    ryz_provisioning_platform_event_sink_t event_sink)
{
    if (event_sink == NULL || s_event_sink != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ryz_provisioning_init_plan_t plan = {0};
    s_init_plan = (ryz_provisioning_init_plan_t){0};
    s_wifi_event_instance = NULL;
    s_ip_event_instance = NULL;
    memset(&s_link_binding, 0, sizeof(s_link_binding));
    s_portal_session = false;
    s_portal_ipv4[0] = '\0';
    s_connect_started_us = 0;
    s_portal_started_us = 0;
    reset_scan_cache();
    ryz_net_traffic_revoke();
    revoke_local_secret_locked();
    wipe_secret(&s_sta_credentials, sizeof(s_sta_credentials));
    portENTER_CRITICAL(&s_command_mux);
    s_network_mode = NETWORK_MODE_STOPPED;
    s_generation = 0;
    s_connect_pending = false;
    s_cancel_requested = false;
    s_cancel_generation = 0;
    s_reconnect_attempts = 0;
    s_reconnect_pending = false;
    s_network_event_pending = false;
    s_network_event_saw_got_ip = false;
    portEXIT_CRITICAL(&s_command_mux);

    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error == ESP_OK) {
        ryz_provisioning_init_acquire(
            &plan, RYZ_PROVISIONING_INIT_EVENT_LOOP);
    } else if (error != ESP_ERR_INVALID_STATE) {
        goto fail;
    }

    error = create_default_wifi_netif(true, &s_ap_netif);
    if (error != ESP_OK) {
        goto fail;
    }
    ryz_provisioning_init_acquire(&plan, RYZ_PROVISIONING_INIT_AP_NETIF);
    error = create_default_wifi_netif(false, &s_sta_netif);
    if (error != ESP_OK) {
        goto fail;
    }
    ryz_provisioning_init_acquire(&plan, RYZ_PROVISIONING_INIT_STA_NETIF);

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_config);
    if (error != ESP_OK) {
        goto fail;
    }
    ryz_provisioning_init_acquire(&plan, RYZ_PROVISIONING_INIT_WIFI);
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error != ESP_OK) {
        goto fail;
    }

    s_network_mutex = xSemaphoreCreateMutex();
    if (s_network_mutex == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_NETWORK_MUTEX);
    s_connect_queue = xQueueCreate(
        RYZ_CONNECT_QUEUE_LENGTH, sizeof(connect_command_t));
    if (s_connect_queue == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_CONNECT_QUEUE);

    if (xTaskCreate(
            connect_worker,
            "ryz_prov",
            4096,
            NULL,
            5,
            &s_connect_task) != pdPASS) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_CONNECT_TASK);

    error = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        network_event_handler,
        NULL,
        &s_wifi_event_instance);
    if (error != ESP_OK) {
        goto fail;
    }
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_WIFI_HANDLER);
    error = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        network_event_handler,
        NULL,
        &s_ip_event_instance);
    if (error != ESP_OK) {
        goto fail;
    }
    ryz_provisioning_init_acquire(
        &plan, RYZ_PROVISIONING_INIT_IP_HANDLER);

    /* Commit only after every owned resource is live. The retained plan lets
     * the core roll back if AP identity preparation fails immediately after
     * this function returns. */
    s_init_plan = plan;
    s_event_sink = event_sink;
    return ESP_OK;

fail:
    cleanup_failed_init(&plan);
    return error;
}
