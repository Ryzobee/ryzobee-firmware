#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/* V5 B01..B08 plus the explicit BLE closure states. The caller selects the page from real service
 * state and owns navigation, BLE, confirmation identity and release gating. */
typedef enum {
    RYZ_V5_BLE_PAIRED,
    RYZ_V5_BLE_OFF,
    RYZ_V5_BLE_EMPTY,
    RYZ_V5_BLE_PAIRING,
    RYZ_V5_BLE_REPLACE,
    RYZ_V5_BLE_SUCCESS,
    RYZ_V5_BLE_INFO,
    RYZ_V5_BLE_FORGET,
} ryz_v5_ble_page_t;

/* Zero preserves existing producers. No radio/business state is synthesized:
 * new values must come from a confirmed backend snapshot, never a UI timer.
 * INFO remains the same seven-field view of that snapshot. */
typedef enum {
    RYZ_V5_BLE_STATE_LEGACY,
    RYZ_V5_BLE_STATE_SAVED_WAITING,
    RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT,
    RYZ_V5_BLE_STATE_PAIR_TIMEOUT,
    RYZ_V5_BLE_STATE_PAIR_FAILED,
    RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE,
    RYZ_V5_BLE_STATE_FORGET_FAILED,
    RYZ_V5_BLE_STATE_SERVICE_FAILED,
    RYZ_V5_BLE_STATE_FORGETTING,
    RYZ_V5_BLE_STATE_VERIFY_CODE,
    RYZ_V5_BLE_STATE_COMMITTING,
    RYZ_V5_BLE_STATE_STOPPING,
} ryz_v5_ble_state_t;

typedef enum {
    RYZ_V5_BLE_FAILURE_UNKNOWN,
    RYZ_V5_BLE_FAILURE_START,
    RYZ_V5_BLE_FAILURE_AUTH,
    RYZ_V5_BLE_FAILURE_SAVE,
} ryz_v5_ble_failure_t;

typedef struct {
    ryz_v5_ble_page_t page;
    ryz_v5_ble_state_t state;
    int info_scroll;
    /* unavailable overrides all purported radio/peer state and forbids every
     * hardware mutation. It does not synthesize OFF, a peer or a countdown. */
    bool unavailable;
    bool enabled;
    bool bonded;
    bool linked;
    bool pairing_active;
    bool rssi_valid;
    int16_t rssi_dbm;
    bool remaining_valid;
    uint16_t remaining_seconds;
    uint16_t pairing_window_seconds; /* 0 = policy not provided. */
    bool reconnect_known;
    bool reconnect_enabled;
    char local_name[33];
    char peer_name[33];
    char local_address[18];
    char mode[33]; /* Actual negotiated/configured mode, never inferred. */
    char role[17];
    bool service_known, service_ready;
    bool bond_known; /* Required before a new state claims no saved bond. */
    bool old_bond_removed; /* Confirmed replacement deletion, not intent. */
    bool checking_state; /* Accepted forget outcome unknown; no retry/back. */
    bool retry_allowed; /* Backend has settled/cleaned up and admits retry. */
    bool replace_after_forget; /* Retain original intent across forget retry. */
    uint32_t operation_id; /* Nonzero accepted operation; Owner rechecks it. */
    bool compare_pending;
    uint32_t compare_value; /* Six decimal digits; capture on physical DOWN. */
    ryz_v5_ble_failure_t failure;
    char error_code[17]; /* Actual service code; absent/invalid displays --. */
} ryz_v5_ble_model_t;

typedef enum {
    RYZ_V5_BLE_ACTION_NONE,
    RYZ_V5_BLE_ACTION_BACK,
    RYZ_V5_BLE_ACTION_ENABLE,
    RYZ_V5_BLE_ACTION_DISABLE,
    RYZ_V5_BLE_ACTION_INFO,
    RYZ_V5_BLE_ACTION_PAIR,
    RYZ_V5_BLE_ACTION_CANCEL_PAIR,
    RYZ_V5_BLE_ACTION_REPLACE,
    RYZ_V5_BLE_ACTION_CONFIRM_REPLACE,
    RYZ_V5_BLE_ACTION_FORGET,
    RYZ_V5_BLE_ACTION_CONFIRM_FORGET,
    RYZ_V5_BLE_ACTION_DONE,
    RYZ_V5_BLE_ACTION_RETRY_PAIR,
    RYZ_V5_BLE_ACTION_RETRY_FORGET,
    RYZ_V5_BLE_ACTION_CONFIRM_CODE,
    RYZ_V5_BLE_ACTION_REJECT_CODE,
} ryz_v5_ble_action_t;

typedef struct {
    esp_err_t (*submit)(void *context, ryz_v5_ble_action_t action,
                        uint32_t operation_id, uint32_t compare_value);
    void *context;
} ryz_v5_ble_binding_t;

/* Owner-only binding. Callback only admits copied commands; no blocking SDK
 * or NVS work on the UI task. NULL restores fail-closed unavailable behavior. */
void ryz_v5_ble_bind_services(const ryz_v5_ble_binding_t *binding);
esp_err_t ryz_v5_ble_submit(ryz_v5_ble_action_t action, uint32_t operation_id,
                           uint32_t compare_value);

/* Trusted Owner-only drawing into a caller-owned candidate LVGL root. Uses
 * shared widgets/fonts/assets, adds no timer/indev and never commits/releases
 * the root or calls services. Call widgets_begin before and widgets_status
 * after the actual LVGL commit; font references live until roots are gone.
 * Model strings are bounded and sanitized to the embedded ASCII repertoire;
 * unknown/empty fields show --, never a fabricated product/device value.
 * These functions do not retain model or root pointers. */
esp_err_t ryz_v5_ble_draw(lv_obj_t *root, const ryz_v5_ble_model_t *model);
int ryz_v5_ble_info_height(const ryz_v5_ble_model_t *model);
const char *ryz_v5_ble_title(const ryz_v5_ble_model_t *model);
/* Pure hit testing only: caller must enforce successful-present, same-frame
 * token and physical release/confirmation rules before delivering an action.
 * Coordinates outside 240x240 and unavailable mutations return NONE. */
ryz_v5_ble_action_t ryz_v5_ble_hit(const ryz_v5_ble_model_t *model, int x, int y);
