#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ryz_provisioning.h"
#include "ryz_provisioning_local_secret.h"

typedef struct {
    char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    char password[RYZ_PROVISIONING_PASSWORD_MAX_LENGTH + 1];
} ryz_provisioning_credentials_t;

typedef struct {
    char ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
} ryz_provisioning_portal_info_t;

typedef enum {
    RYZ_PROVISIONING_PLATFORM_CREDENTIALS_RECEIVED = 0,
    RYZ_PROVISIONING_PLATFORM_CONNECTING,
    RYZ_PROVISIONING_PLATFORM_ONLINE,
    RYZ_PROVISIONING_PLATFORM_FAILED,
    RYZ_PROVISIONING_PLATFORM_LINK_INFO,
    /** The STA attempt was cancelled while the local AP portal stayed alive. */
    RYZ_PROVISIONING_PLATFORM_PORTAL_READY,
    /** Trusted boot fallback is starting AP; retains saved credentials/intent. */
    RYZ_PROVISIONING_PLATFORM_BOOT_AP_STARTING,
} ryz_provisioning_platform_event_type_t;

typedef struct {
    ryz_provisioning_platform_event_type_t type;
    esp_err_t error;
    int32_t detail;
    char ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    char ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
    /** Diagnostic update only; ssid/ipv4 bind the previously verified ONLINE
     * identity. Must never independently promote a connection to ONLINE. */
    ryz_provisioning_link_info_t link;
} ryz_provisioning_platform_event_t;

typedef void (*ryz_provisioning_platform_event_sink_t)(
    const ryz_provisioning_platform_event_t *event);

void ryz_provisioning_platform_snapshot_lock(void);
void ryz_provisioning_platform_snapshot_unlock(void);

esp_err_t ryz_provisioning_platform_init(
    ryz_provisioning_platform_event_sink_t event_sink);
/** Roll back a successful platform init that has not entered service yet. */
void ryz_provisioning_platform_deinit(void);
/** Read the explicit ON/OFF preference after platform init. NOT_FOUND means
 * no saved choice, not storage failure. On any error, out_enabled is false. */
esp_err_t ryz_provisioning_platform_load_enabled(bool *out_enabled);
/** Save/commit only the ON/OFF preference; no radio or credential mutation.
 * Failure does not prove that storage retained its previous value. */
esp_err_t ryz_provisioning_platform_save_enabled(bool enabled);
esp_err_t ryz_provisioning_platform_get_ap_identity(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size);
esp_err_t ryz_provisioning_platform_load_credentials(
    ryz_provisioning_credentials_t *credentials);
/** On success, returns the IPv4 address of the AP netif in out_portal_info. */
esp_err_t ryz_provisioning_platform_start_portal(
    ryz_provisioning_portal_info_t *out_portal_info);
esp_err_t ryz_provisioning_platform_start_sta(
    const ryz_provisioning_credentials_t *credentials);
/** Boot owner only: start the STA radio without associating, perform a
 * directed SSID scan, release driver scan records, then emit CONNECTING and
 * connect only on an exact match. NOT_FOUND means a completed scan with no
 * match; other errors are not absence. Serialized with portal/STA operations.
 * No snapshot lock is held during radio I/O; no credential persistence. */
esp_err_t ryz_provisioning_platform_start_saved_sta(
    const ryz_provisioning_credentials_t *credentials);
/** Called only for the admitted first saved-STA attempt. Atomically validates
 * live STA and publishes ONLINE, or revokes the old generation and starts AP.
 * Emits BOOT_AP_STARTING/PORTAL_READY while owning the network mutex. */
esp_err_t ryz_provisioning_platform_boot_fallback(void);
esp_err_t ryz_provisioning_platform_clear_credentials(void);
/** Stop all owned portal/radio services without deleting any credentials.
 * Success acknowledges shutdown; failure retains uncertain handles for retry. */
esp_err_t ryz_provisioning_platform_stop(void);

/* Private adapters for the trusted local-secret API; no public snapshot data. */
esp_err_t ryz_provisioning_platform_local_secret_try_get(
    ryz_provisioning_local_secret_t *out);
esp_err_t ryz_provisioning_platform_local_secret_try_copy(
    uint64_t expected_epoch, char *out, size_t cap);
