#include "ryz_provisioning.h"

#include <string.h>
#include <stdatomic.h>

#include "ryz_provisioning_platform.h"

static atomic_bool s_initialized;
static ryz_provisioning_snapshot_t s_snapshot;
static atomic_flag s_control = ATOMIC_FLAG_INIT;
/* Control-owner cache of the last confirmed persistent choice. This is not
 * the current radio state: temporary setup may enable a saved-OFF device. */
static bool s_preference_known, s_saved_enabled;
/* Control-owner only. Explicit user operations supersede this boot attempt;
 * normal retries/drops must never gain a fresh AP fallback opportunity. */
static bool s_boot_start_seen;

/* Only one trusted control operation; no lock is held over event callbacks.
 * A caller competing with the owner must retry explicitly, never spin. */
static bool enter_control(void)
{ return !atomic_flag_test_and_set_explicit(&s_control, memory_order_acquire); }
static void leave_control(void)
{ atomic_flag_clear_explicit(&s_control, memory_order_release); }

static void copy_string(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    size_t length = strnlen(source, destination_size - 1);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void publish_failed(esp_err_t error, int32_t detail)
{
    ryz_provisioning_platform_snapshot_lock();
    if (s_snapshot.boot_phase == RYZ_PROVISIONING_BOOT_SCANNING)
        s_snapshot.boot_phase = error == ESP_ERR_NOT_FOUND ?
            RYZ_PROVISIONING_BOOT_NOT_FOUND : RYZ_PROVISIONING_BOOT_SCAN_FAILED;
    else if (s_snapshot.boot_phase == RYZ_PROVISIONING_BOOT_CONNECTING)
        s_snapshot.boot_phase = RYZ_PROVISIONING_BOOT_CONNECT_FAILED;
    s_snapshot.state = RYZ_PROVISIONING_FAILED;
    s_snapshot.last_error = error;
    s_snapshot.detail = detail;
    memset(&s_snapshot.link, 0, sizeof(s_snapshot.link));
    s_snapshot.portal_active = false;
    s_snapshot.portal_ip[0] = '\0';
    ++s_snapshot.revision;
    ryz_provisioning_platform_snapshot_unlock();
}

static esp_err_t start_portal(ryz_provisioning_portal_info_t *out_portal_info)
{
    memset(out_portal_info, 0, sizeof(*out_portal_info));
    esp_err_t error = ryz_provisioning_platform_start_portal(out_portal_info);
    if (error == ESP_OK && out_portal_info->ipv4[0] == '\0') {
        return ESP_FAIL;
    }
    return error;
}

static void publish_portal_ready(uint32_t starting_revision,
                                  const ryz_provisioning_portal_info_t *portal)
{
    ryz_provisioning_platform_snapshot_lock();
    /* HTTP becomes usable before start_portal() returns. Never overwrite a
     * credential/STA event that already advanced this exact AP transition. */
    if (s_snapshot.enabled && s_snapshot.state == RYZ_PROVISIONING_AP_STARTING &&
        s_snapshot.revision == starting_revision) {
        s_snapshot.state = RYZ_PROVISIONING_AP_READY;
        s_snapshot.portal_active = true;
        copy_string(s_snapshot.portal_ip, sizeof(s_snapshot.portal_ip), portal->ipv4);
        ++s_snapshot.revision;
    }
    ryz_provisioning_platform_snapshot_unlock();
}

static void receive_platform_event(const ryz_provisioning_platform_event_t *event)
{
    if (event == NULL || !s_initialized) {
        return;
    }

    ryz_provisioning_platform_snapshot_lock();
    if (!s_snapshot.enabled) {
        ryz_provisioning_platform_snapshot_unlock();
        return;
    }
    if (event->type == RYZ_PROVISIONING_PLATFORM_LINK_INFO) {
        /* Platform owns generation provenance. This extra check accepts only
         * the current ONLINE SSID/address and never rolls its sample backward;
         * diagnostic data alone cannot establish a new connection. */
        if (s_snapshot.state == RYZ_PROVISIONING_ONLINE &&
            s_snapshot.sta_ssid[0] && s_snapshot.ipv4[0] &&
            strncmp(s_snapshot.sta_ssid, event->ssid, sizeof(event->ssid)) == 0 &&
            strncmp(s_snapshot.ipv4, event->ipv4, sizeof(event->ipv4)) == 0 &&
            event->link.sampled_at_us >= s_snapshot.link.sampled_at_us) {
            s_snapshot.link = event->link;
            /* The fixed buffers remain safe even for a malformed platform
             * observation; invalid flags never preserve previous strings. */
            s_snapshot.link.mac[sizeof(s_snapshot.link.mac) - 1U] = '\0';
            s_snapshot.link.ipv6[sizeof(s_snapshot.link.ipv6) - 1U] = '\0';
            if (!s_snapshot.link.mac_valid) s_snapshot.link.mac[0] = '\0';
            if (!s_snapshot.link.ipv6_valid) s_snapshot.link.ipv6[0] = '\0';
            if (!s_snapshot.link.rssi_valid) s_snapshot.link.rssi_dbm = 0;
            ++s_snapshot.revision;
        }
        ryz_provisioning_platform_snapshot_unlock();
        return;
    }
    memset(&s_snapshot.link, 0, sizeof(s_snapshot.link));
    switch (event->type) {
    case RYZ_PROVISIONING_PLATFORM_CREDENTIALS_RECEIVED:
        s_snapshot.boot_restore_pending = false;
        s_snapshot.state = RYZ_PROVISIONING_CREDENTIALS_RECEIVED;
        s_snapshot.credentials_stored = true;
        s_snapshot.last_error = ESP_OK;
        s_snapshot.detail = 0;
        copy_string(s_snapshot.sta_ssid, sizeof(s_snapshot.sta_ssid), event->ssid);
        s_snapshot.ipv4[0] = '\0';
        break;
    case RYZ_PROVISIONING_PLATFORM_CONNECTING:
        if (s_snapshot.boot_restore_pending)
            s_snapshot.boot_phase = RYZ_PROVISIONING_BOOT_CONNECTING;
        s_snapshot.state = RYZ_PROVISIONING_CONNECTING;
        s_snapshot.portal_active = false;
        s_snapshot.portal_ip[0] = '\0';
        s_snapshot.ipv4[0] = '\0';
        s_snapshot.last_error = ESP_OK;
        s_snapshot.detail = 0;
        break;
    case RYZ_PROVISIONING_PLATFORM_ONLINE:
        if (s_snapshot.boot_phase == RYZ_PROVISIONING_BOOT_CONNECTING ||
            s_snapshot.boot_phase == RYZ_PROVISIONING_BOOT_CONNECT_FAILED)
            s_snapshot.boot_phase = RYZ_PROVISIONING_BOOT_CONNECTED;
        s_snapshot.boot_restore_pending = false;
        s_snapshot.state = RYZ_PROVISIONING_ONLINE;
        s_snapshot.credentials_stored = true;
        s_snapshot.portal_active = false;
        s_snapshot.portal_ip[0] = '\0';
        s_snapshot.last_error = ESP_OK;
        s_snapshot.detail = 0;
        copy_string(s_snapshot.sta_ssid, sizeof(s_snapshot.sta_ssid), event->ssid);
        copy_string(s_snapshot.ipv4, sizeof(s_snapshot.ipv4), event->ipv4);
        break;
    case RYZ_PROVISIONING_PLATFORM_FAILED:
        if (s_snapshot.boot_phase == RYZ_PROVISIONING_BOOT_CONNECTING)
            s_snapshot.boot_phase = RYZ_PROVISIONING_BOOT_CONNECT_FAILED;
        s_snapshot.state = RYZ_PROVISIONING_FAILED;
        s_snapshot.portal_active = false;
        s_snapshot.portal_ip[0] = '\0';
        s_snapshot.last_error = event->error != ESP_OK ? event->error : ESP_FAIL;
        s_snapshot.detail = event->detail;
        s_snapshot.ipv4[0] = '\0';
        break;
    case RYZ_PROVISIONING_PLATFORM_PORTAL_READY:
        /* A target-side cancel keeps the AP/HTTP/DNS services running. This
         * event is deliberately distinct from AP_STARTING: it cannot erase
         * credentials or imply that the station is online. */
        s_snapshot.state = RYZ_PROVISIONING_AP_READY;
        s_snapshot.stop_confirmed = false;
        s_snapshot.portal_active = true;
        copy_string(s_snapshot.portal_ip, sizeof(s_snapshot.portal_ip), event->ipv4);
        s_snapshot.last_error = ESP_OK;
        s_snapshot.detail = 0;
        s_snapshot.ipv4[0] = '\0';
        break;
    case RYZ_PROVISIONING_PLATFORM_BOOT_AP_STARTING:
        if (s_snapshot.boot_phase == RYZ_PROVISIONING_BOOT_CONNECTING)
            s_snapshot.boot_phase = RYZ_PROVISIONING_BOOT_CONNECT_FAILED;
        s_snapshot.state = RYZ_PROVISIONING_AP_STARTING;
        s_snapshot.stop_confirmed = false;
        s_snapshot.portal_active = false;
        s_snapshot.portal_ip[0] = '\0';
        s_snapshot.ipv4[0] = '\0';
        s_snapshot.last_error = ESP_OK;
        s_snapshot.detail = 0;
        break;
    default:
        s_snapshot.state = RYZ_PROVISIONING_FAILED;
        s_snapshot.portal_active = false;
        s_snapshot.portal_ip[0] = '\0';
        s_snapshot.last_error = ESP_ERR_INVALID_ARG;
        s_snapshot.detail = (int32_t)event->type;
        break;
    }
    ++s_snapshot.revision;
    ryz_provisioning_platform_snapshot_unlock();
}

esp_err_t ryz_provisioning_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t error = ryz_provisioning_platform_init(receive_platform_event);
    if (error != ESP_OK) {
        return error;
    }

    bool enabled = false;
    error = ryz_provisioning_platform_load_enabled(&enabled);
    const bool preference_known = error == ESP_OK;
    if (error == ESP_ERR_NOT_FOUND) enabled = true; /* First boot / older firmware. */
    else if (error != ESP_OK) {
        /* A bad/unreadable record is not permission to secretly enable Wi-Fi. */
        ryz_provisioning_platform_deinit();
        return error;
    }

    ryz_provisioning_snapshot_t initial = {
        .state = enabled ? RYZ_PROVISIONING_UNCONFIGURED : RYZ_PROVISIONING_OFF,
        .revision = 1,
        .last_error = ESP_OK,
        .enabled = enabled,
        .stop_confirmed = true,
        .boot_phase = enabled ? RYZ_PROVISIONING_BOOT_WAITING : RYZ_PROVISIONING_BOOT_OFF,
    };
    error = ryz_provisioning_platform_get_ap_identity(
        initial.ap_ssid,
        sizeof(initial.ap_ssid),
        initial.ap_password,
        sizeof(initial.ap_password));
    if (error != ESP_OK) {
        ryz_provisioning_platform_deinit();
        return error;
    }

    ryz_provisioning_platform_snapshot_lock();
    s_snapshot = initial;
    s_preference_known = preference_known;
    s_saved_enabled = enabled;
    s_initialized = true;
    ryz_provisioning_platform_snapshot_unlock();
    return ESP_OK;
}

static esp_err_t start_network(bool boot_attempt)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ryz_provisioning_platform_snapshot_lock();
    ryz_provisioning_state_t current_state = s_snapshot.state;
    bool enabled = s_snapshot.enabled;
    ryz_provisioning_platform_snapshot_unlock();
    if (!enabled) return ESP_OK;
    if (current_state != RYZ_PROVISIONING_UNCONFIGURED &&
        current_state != RYZ_PROVISIONING_FAILED) {
        /* The platform owns one Wi-Fi session. Avoid stop/start cycling an
         * active STA merely because start() was called again: delayed stop
         * callbacks cannot then be mistaken for a new STA session. */
        return ESP_OK;
    }

    ryz_provisioning_credentials_t credentials = {0};
    esp_err_t error = ryz_provisioning_platform_load_credentials(&credentials);
    if (error == ESP_ERR_NOT_FOUND) {
        ryz_provisioning_platform_snapshot_lock();
        if (boot_attempt) s_snapshot.boot_phase = RYZ_PROVISIONING_BOOT_NO_CREDENTIALS;
        s_snapshot.state = RYZ_PROVISIONING_AP_STARTING;
        s_snapshot.stop_confirmed = false;
        s_snapshot.credentials_stored = false;
        s_snapshot.portal_active = false;
        s_snapshot.portal_ip[0] = '\0';
        s_snapshot.sta_ssid[0] = '\0';
        s_snapshot.ipv4[0] = '\0';
        memset(&s_snapshot.link, 0, sizeof(s_snapshot.link));
        s_snapshot.last_error = ESP_OK;
        s_snapshot.detail = 0;
        ++s_snapshot.revision;
        const uint32_t starting_revision = s_snapshot.revision;
        ryz_provisioning_platform_snapshot_unlock();

        ryz_provisioning_portal_info_t portal_info = {0};
        error = start_portal(&portal_info);
        if (error != ESP_OK) {
            publish_failed(error, 0);
            return error;
        }

        publish_portal_ready(starting_revision, &portal_info);
        return ESP_OK;
    }
    if (error != ESP_OK) {
        publish_failed(error, 0);
        return error;
    }

    ryz_provisioning_platform_snapshot_lock();
    s_snapshot.state = RYZ_PROVISIONING_CONNECTING;
    s_snapshot.stop_confirmed = false;
    s_snapshot.credentials_stored = true;
    /* Set admission from the selected saved-STA branch, never from a later
     * snapshot that may already represent a fast HTTP submission on first AP. */
    s_snapshot.boot_restore_pending = boot_attempt;
    if (boot_attempt) s_snapshot.boot_phase = RYZ_PROVISIONING_BOOT_SCANNING;
    s_snapshot.portal_active = false;
    s_snapshot.portal_ip[0] = '\0';
    s_snapshot.last_error = ESP_OK;
    s_snapshot.detail = 0;
    copy_string(s_snapshot.sta_ssid, sizeof(s_snapshot.sta_ssid), credentials.ssid);
    s_snapshot.ipv4[0] = '\0';
    memset(&s_snapshot.link, 0, sizeof(s_snapshot.link));
    ++s_snapshot.revision;
    ryz_provisioning_platform_snapshot_unlock();

    error = boot_attempt ? ryz_provisioning_platform_start_saved_sta(&credentials) :
        ryz_provisioning_platform_start_sta(&credentials);
    if (error != ESP_OK) {
        publish_failed(error, 0);
    }
    return error;
}

esp_err_t ryz_provisioning_start(void)
{
    if (!enter_control()) return ESP_ERR_INVALID_STATE;
    const bool first_start = s_initialized && !s_boot_start_seen;
    if (first_start) s_boot_start_seen = true;
    esp_err_t error = start_network(first_start);
    leave_control();
    return error;
}

esp_err_t ryz_provisioning_boot_fallback(void)
{
    if (!s_initialized || !enter_control()) return ESP_ERR_INVALID_STATE;
    ryz_provisioning_platform_snapshot_lock();
    const bool eligible = s_snapshot.boot_restore_pending && s_snapshot.enabled &&
        s_snapshot.credentials_stored &&
        (s_snapshot.state == RYZ_PROVISIONING_CONNECTING ||
         s_snapshot.state == RYZ_PROVISIONING_FAILED);
    /* One attempt, including an error. Only an explicit later user control
     * may retry a failed radio transition; background cannot keep opening AP. */
    if (s_snapshot.boot_restore_pending) {
        s_snapshot.boot_restore_pending = false;
        ++s_snapshot.revision;
    }
    ryz_provisioning_platform_snapshot_unlock();
    esp_err_t error = eligible ? ryz_provisioning_platform_boot_fallback() : ESP_OK;
    /* The platform publishes the exact result under its network lock. Never
     * overwrite a newer IP/HTTP event after it releases that lock. */
    leave_control();
    return error;
}

static void revoke_boot_restore(void)
{
    s_boot_start_seen = true;
    ryz_provisioning_platform_snapshot_lock();
    if (s_snapshot.boot_restore_pending) {
        s_snapshot.boot_restore_pending = false;
        ++s_snapshot.revision;
    }
    ryz_provisioning_platform_snapshot_unlock();
}

static esp_err_t stop_network(void)
{
    ryz_provisioning_platform_snapshot_lock();
    s_snapshot.enabled = false;
    bool stopped = s_snapshot.stop_confirmed;
    s_snapshot.state = stopped ? RYZ_PROVISIONING_OFF : RYZ_PROVISIONING_STOPPING;
    s_snapshot.ipv4[0] = '\0';
    memset(&s_snapshot.link, 0, sizeof(s_snapshot.link));
    s_snapshot.last_error = ESP_OK;
    s_snapshot.detail = 0;
    ++s_snapshot.revision;
    ryz_provisioning_platform_snapshot_unlock();

    esp_err_t error = stopped ? ESP_OK : ryz_provisioning_platform_stop();
    ryz_provisioning_platform_snapshot_lock();
    s_snapshot.stop_confirmed = error == ESP_OK;
    s_snapshot.state = error == ESP_OK ? RYZ_PROVISIONING_OFF : RYZ_PROVISIONING_FAILED;
    s_snapshot.last_error = error;
    /* Failed stop does not assert that a still-owned portal handle disappeared.
     * The phase/error and stop_confirmed distinguish that last-known view. */
    if (error == ESP_OK) {
        s_snapshot.portal_active = false;
        s_snapshot.portal_ip[0] = '\0';
    }
    ++s_snapshot.revision;
    ryz_provisioning_platform_snapshot_unlock();
    return error;
}

esp_err_t ryz_provisioning_set_enabled(bool enabled)
{
    if (!s_initialized || !enter_control()) return ESP_ERR_INVALID_STATE;
    revoke_boot_restore();
    esp_err_t error = ESP_OK;
    if (!s_preference_known || s_saved_enabled != enabled) {
        /* Persist the explicit intent before changing radio ownership. NVS
         * errors must not become successful toggles. A failed commit can be
         * ambiguous, so no later retry may rely on our previous cached value. */
        error = ryz_provisioning_platform_save_enabled(enabled);
        s_preference_known = error == ESP_OK;
        if (error != ESP_OK) {
            leave_control();
            return error;
        }
        s_saved_enabled = enabled;
    }
    /* Skipping an unchanged NVS write never skips the actual radio operation.
     * Runtime failures retain the committed preference; no hidden rollback. */
    if (!enabled) error = stop_network();
    else {
        ryz_provisioning_platform_snapshot_lock();
        bool was_enabled = s_snapshot.enabled;
        ryz_provisioning_platform_snapshot_unlock();
        if (!was_enabled) {
            error = stop_network();
            if (error == ESP_OK) {
                ryz_provisioning_platform_snapshot_lock();
                s_snapshot.enabled = true;
                s_snapshot.state = RYZ_PROVISIONING_UNCONFIGURED;
                ++s_snapshot.revision;
                ryz_provisioning_platform_snapshot_unlock();
            }
        }
        if (error == ESP_OK) error = start_network(false);
    }
    leave_control();
    return error;
}

static void clear_secret(void *buffer, size_t size);

esp_err_t ryz_provisioning_open_portal(void)
{
    if (!s_initialized || !enter_control()) return ESP_ERR_INVALID_STATE;
    revoke_boot_restore();
    ryz_provisioning_platform_snapshot_lock();
    const bool ready = s_snapshot.enabled && !s_snapshot.stop_confirmed &&
        s_snapshot.state == RYZ_PROVISIONING_AP_READY && s_snapshot.portal_active &&
        s_snapshot.portal_ip[0] && s_snapshot.last_error == ESP_OK;
    ryz_provisioning_platform_snapshot_unlock();
    if (ready) {
        leave_control();
        return ESP_OK;
    }

    /* Revoke online/secret admission before waiting on the network owner.
     * Opening AP is not forgetting: neither this function nor its error paths
     * call a credential mutation. A previous HTTP submission remains governed
     * by the portal's existing persistence policy. */
    ryz_provisioning_credentials_t credentials = {0};
    esp_err_t error = stop_network();
    if (error != ESP_OK) goto done;
    error = ryz_provisioning_platform_load_credentials(&credentials);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        publish_failed(error, 0);
        goto done;
    }

    ryz_provisioning_platform_snapshot_lock();
    s_snapshot.enabled = true;
    s_snapshot.stop_confirmed = false;
    s_snapshot.state = RYZ_PROVISIONING_AP_STARTING;
    s_snapshot.credentials_stored = error == ESP_OK;
    copy_string(s_snapshot.sta_ssid, sizeof(s_snapshot.sta_ssid),
                error == ESP_OK ? credentials.ssid : NULL);
    s_snapshot.portal_active = false;
    s_snapshot.portal_ip[0] = '\0';
    s_snapshot.ipv4[0] = '\0';
    memset(&s_snapshot.link, 0, sizeof(s_snapshot.link));
    s_snapshot.last_error = ESP_OK;
    s_snapshot.detail = 0;
    ++s_snapshot.revision;
    const uint32_t starting_revision = s_snapshot.revision;
    ryz_provisioning_platform_snapshot_unlock();
    clear_secret(&credentials, sizeof(credentials));

    ryz_provisioning_portal_info_t portal_info = {0};
    error = start_portal(&portal_info);
    if (error != ESP_OK) publish_failed(error, 0);
    else publish_portal_ready(starting_revision, &portal_info);
done:
    clear_secret(&credentials, sizeof(credentials));
    leave_control();
    return error;
}

esp_err_t ryz_provisioning_get_snapshot(ryz_provisioning_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ryz_provisioning_platform_snapshot_lock();
    *out_snapshot = s_snapshot;
    ryz_provisioning_platform_snapshot_unlock();
    return ESP_OK;
}

static bool local_secret_allowed(void)
{
    ryz_provisioning_platform_snapshot_lock();
    bool allowed = s_initialized && s_snapshot.enabled &&
        s_snapshot.state == RYZ_PROVISIONING_ONLINE;
    ryz_provisioning_platform_snapshot_unlock();
    return allowed;
}

static void clear_secret(void *buffer, size_t size)
{
    volatile unsigned char *bytes = buffer;
    while (size--) *bytes++ = 0;
}

esp_err_t ryz_provisioning_local_secret_try_get(ryz_provisioning_local_secret_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!local_secret_allowed()) return ESP_OK;
    esp_err_t error = ryz_provisioning_platform_local_secret_try_get(out);
    if (error != ESP_OK || !local_secret_allowed()) memset(out, 0, sizeof(*out));
    return error;
}

esp_err_t ryz_provisioning_local_secret_try_copy(uint64_t expected_epoch, char *out, size_t cap)
{
    if (out != NULL && cap != 0) clear_secret(out, cap);
    if (out == NULL || cap == 0 || expected_epoch == 0) return ESP_ERR_INVALID_ARG;
    if (!local_secret_allowed()) return ESP_ERR_INVALID_STATE;
    /* Snapshot critical sections never span the platform's try-lock. A stop
     * publishes disabled before it waits for the network owner; recheck after
     * copying so that this early revocation cannot leave a returned password. */
    esp_err_t error = ryz_provisioning_platform_local_secret_try_copy(expected_epoch, out, cap);
    if (error == ESP_OK && !local_secret_allowed()) error = ESP_ERR_INVALID_STATE;
    if (error != ESP_OK) clear_secret(out, cap);
    return error;
}

static esp_err_t reprovision_network(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The legacy explicit forget-and-open action also opts back into Wi-Fi.
     * First confirm shutdown: a failed stop must never erase credentials. */
    esp_err_t error = stop_network();
    if (error != ESP_OK) return error;

    error = ryz_provisioning_platform_clear_credentials();
    if (error != ESP_OK) {
        publish_failed(error, 0);
        return error;
    }

    ryz_provisioning_platform_snapshot_lock();
    s_snapshot.enabled = true;
    s_snapshot.stop_confirmed = false;
    s_snapshot.state = RYZ_PROVISIONING_AP_STARTING;
    s_snapshot.credentials_stored = false;
    s_snapshot.portal_active = false;
    s_snapshot.portal_ip[0] = '\0';
    s_snapshot.sta_ssid[0] = '\0';
    s_snapshot.ipv4[0] = '\0';
    memset(&s_snapshot.link, 0, sizeof(s_snapshot.link));
    s_snapshot.last_error = ESP_OK;
    s_snapshot.detail = 0;
    ++s_snapshot.revision;
    const uint32_t starting_revision = s_snapshot.revision;
    ryz_provisioning_platform_snapshot_unlock();

    ryz_provisioning_portal_info_t portal_info = {0};
    error = start_portal(&portal_info);
    if (error != ESP_OK) {
        publish_failed(error, 0);
        return error;
    }

    publish_portal_ready(starting_revision, &portal_info);
    return ESP_OK;
}

esp_err_t ryz_provisioning_request_reprovision(void)
{
    if (!enter_control()) return ESP_ERR_INVALID_STATE;
    if (s_initialized) revoke_boot_restore();
    esp_err_t error = reprovision_network();
    leave_control();
    return error;
}
