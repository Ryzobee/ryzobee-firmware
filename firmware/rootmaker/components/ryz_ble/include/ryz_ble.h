#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RYZ_BLE_OFF = 0,
    RYZ_BLE_EMPTY,
    RYZ_BLE_SAVED_WAITING,
    RYZ_BLE_PAIRING,
    RYZ_BLE_COMPARISON,
    RYZ_BLE_COMMITTING,
    RYZ_BLE_PAIRED,
    RYZ_BLE_LINKED,
    RYZ_BLE_PAIR_TIMEOUT,
    RYZ_BLE_PAIR_FAILED,
    RYZ_BLE_STOPPING,
    RYZ_BLE_FORGETTING,
    RYZ_BLE_FORGET_FAILED,
    RYZ_BLE_FAILED,
} ryz_ble_phase_t;

typedef enum {
    RYZ_BLE_FAILURE_NONE = 0,
    RYZ_BLE_FAILURE_START,
    RYZ_BLE_FAILURE_AUTH,
    RYZ_BLE_FAILURE_SAVE,
    RYZ_BLE_FAILURE_STOP,
    RYZ_BLE_FAILURE_LOAD,
} ryz_ble_failure_t;

typedef enum {
    RYZ_BLE_ENABLE = 0,
    RYZ_BLE_DISABLE,
    RYZ_BLE_PAIR_NEW,
    RYZ_BLE_CANCEL_PAIR,
    RYZ_BLE_FORGET,
    RYZ_BLE_REPLACE,
    RYZ_BLE_RETRY_PAIR,
    RYZ_BLE_RETRY_FORGET,
} ryz_ble_action_t;

/* Discovery only. DISCOVERED does not mean notification access was granted,
 * a notification subscription exists, or an application data channel is ready. */
typedef enum {
    RYZ_BLE_ANCS_IDLE = 0,
    RYZ_BLE_ANCS_DISCOVERING,
    RYZ_BLE_ANCS_DISCOVERED,
    RYZ_BLE_ANCS_UNAVAILABLE,
    RYZ_BLE_ANCS_ERROR,
} ryz_ble_ancs_state_t;

/* Consumer Control only. Adding HID does not make other Lua applications
 * use HID or expose radio configuration/raw reports to them. */
typedef enum {
    RYZ_BLE_HID_PLAY_PAUSE = 0,
    RYZ_BLE_HID_STOP,
    RYZ_BLE_HID_NEXT_TRACK,
    RYZ_BLE_HID_PREVIOUS_TRACK,
    RYZ_BLE_HID_MUTE,
    RYZ_BLE_HID_VOLUME_UP,
    RYZ_BLE_HID_VOLUME_DOWN,
} ryz_ble_hid_key_t;

/** Copy-only, bounded public data; never includes keys, connection handles or
 * raw SDK pointers. linked is GAP connection, authenticated additionally
 * requires encrypted Secure Connections/MITM and a verified durable bond.
 * No application GATT protocol exists: service_known/ready always remain false.
 * saved_enabled is persistent user intent; enabled is the current boot radio
 * policy (automatic failed boot restore turns only this value off). */
typedef struct {
    uint32_t revision;
    uint32_t operation_id;
    ryz_ble_phase_t phase;
    ryz_ble_failure_t failure;
    esp_err_t last_error;
    int32_t sdk_error;
    bool available;
    bool enabled;
    bool saved_enabled;
    bool preference_known;
    bool bond_known;
    bool bonded;
    bool linked;
    bool authenticated;
    bool service_known;
    bool service_ready;
    ryz_ble_ancs_state_t ancs_state;
    int32_t ancs_sdk_error;
    bool ancs_service_changed_subscribed;
    bool hid_ready; /* Secure link + subscribed consumer report, not app readiness. */
    bool hid_busy;  /* Queued tap, held report, or cleanup still in progress. */
    int32_t hid_sdk_error;
    bool compare_pending;
    uint32_t compare_value; /* Exactly six decimal digits, including leading 0. */
    bool remaining_valid;
    uint16_t remaining_s;
    bool rssi_valid;
    int8_t rssi_dbm;
    bool boot_settled;
    esp_err_t boot_error;
    bool checking_state;
    bool retry_allowed;
    bool replace_after_forget;
    bool old_bond_removed;
    bool command_pending;
    char local_name[33];
    char local_address[18];
    char peer_name[33]; /* "Phone/PC" unless a real peer name is known. */
    char peer_address[18];
} ryz_ble_snapshot_t;

/** Bounded startup instrumentation; no addresses, peer data or keys. A zero
 * result is meaningful only when returned=true. Heap values are bytes of
 * MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT, not external PSRAM or total RAM. */
typedef struct {
    bool entered;
    bool returned;
    esp_err_t result;
    uint32_t internal_free_before;
    uint32_t internal_largest_before;
    uint32_t internal_free_after;
    uint32_t internal_largest_after;
} ryz_ble_init_probe_t;

typedef struct {
    bool finished;
    ryz_ble_init_probe_t port;
    ryz_ble_init_probe_t buffers;
    ryz_ble_init_probe_t vhci;
} ryz_ble_init_diagnostics_t;

/** Start asynchronous C ownership once. Creates its own bootstrap/host task;
 * no NVS, pairing or radio wait on the caller/UI task. ESP_OK means admitted,
 * not ready. Startup errors and the monotonic boot milestone are in snapshot. */
esp_err_t ryz_ble_start(void);

/** Coherent copy; available=false before startup. Safe from UI and Lua tasks. */
esp_err_t ryz_ble_get_snapshot(ryz_ble_snapshot_t *out);

/** Coherent first-initialization diagnostics. Before the SDK is called all
 * probes are unentered; a failed prerequisite is not reported as SDK success.
 * Does not initiate a radio operation or retry initialization. */
esp_err_t ryz_ble_get_init_diagnostics(ryz_ble_init_diagnostics_t *out);

/** Nonblocking command admission. expected_operation_id must exactly match the
 * observed snapshot. FORGET/REPLACE mean the firmware UI already confirmed the
 * destructive intent. Admission is not completion; observe phase/error/revision.
 * Busy/COMMITTING rejects rather than accepting a cancellation it cannot honor. */
esp_err_t ryz_ble_request(ryz_ble_action_t action, uint32_t expected_operation_id);

/** Numeric Comparison confirmation, bound to operation AND displayed number.
 * The owner rechecks peer/session/deadline before injecting accept/reject.
 * Reject/cancel revokes candidate key authorization before it is queued. */
esp_err_t ryz_ble_confirm_numeric(uint32_t expected_operation_id,
                                uint32_t expected_value, bool accept);

/** Allocate a single, monotonically identified input lease for a Lua job.
 * No radio operation or key is sent by opening it. Returns NOT_FINISHED if
 * another lease or its asynchronous key-release cleanup still owns input. */
esp_err_t ryz_ble_hid_open(uint32_t *session_id);

/** Admit one bounded press/release. ESP_OK means queued, not phone delivery.
 * Requires the current lease and a secure, subscribed HID link; does not
 * connect, pair or change identity. No raw report or descriptor API. */
esp_err_t ryz_ble_hid_tap(uint32_t session_id, ryz_ble_hid_key_t key);

/** Revoke a job immediately, cancel a queued tap and request a held report's
 * release. A tap already dispatched to the host cannot be unsent; cleanup
 * still requests its release. Idempotent; a stale token cannot
 * affect a later job. Host owner finishes release or disconnects on failure. */
void ryz_ble_hid_close(uint32_t session_id);

#ifdef __cplusplus
}
#endif
