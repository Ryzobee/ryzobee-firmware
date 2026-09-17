#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "ryz_system_ui.h"

typedef enum {
    RYZ_V5_NETWORK_CONNECTED = 0,
    RYZ_V5_NETWORK_OFF,
    RYZ_V5_NETWORK_INFO,
    RYZ_V5_NETWORK_AP,
    RYZ_V5_NETWORK_JOINING,
    RYZ_V5_NETWORK_FAILED,
    RYZ_V5_NETWORK_READY,
    RYZ_V5_NETWORK_FILE_SERVER,
    RYZ_V5_NETWORK_NO_NETWORK,
} ryz_v5_network_page_t;

typedef enum {
    RYZ_V5_NETWORK_ACTION_NONE = 0,
    RYZ_V5_NETWORK_ACTION_ON,
    RYZ_V5_NETWORK_ACTION_OFF,
    RYZ_V5_NETWORK_ACTION_INFO,
    RYZ_V5_NETWORK_ACTION_REPROVISION,
    RYZ_V5_NETWORK_ACTION_FILES,
    RYZ_V5_NETWORK_ACTION_BACK,
    RYZ_V5_NETWORK_ACTION_HOME,
    RYZ_V5_NETWORK_ACTION_CANCEL,
    RYZ_V5_NETWORK_ACTION_RETRY,
    RYZ_V5_NETWORK_ACTION_HOLD_PASSWORD,
} ryz_v5_network_action_t;

/* Trusted Owner-only copy. As with system.ap_password, this view must never
 * enter logs/RPC. Neither draw nor hit reads hardware or executes services.
 * enabled is the actual current-boot radio intent, not inferred from FAILED.
 * busy means the service is unavailable or has an uncompleted command. An unavailable
 * elapsed clock is represented by elapsed_valid=false, never sample data. */
typedef struct {
    ryz_v5_network_page_t page;
    int info_scroll;
    ryz_system_ui_snapshot_t system;
    bool enabled;
    bool busy;
    /* Separate intent from the still-cached link while the service owner
     * waits for OTA cleanup or switches interfaces. */
    bool setup_pending;
    bool setup_failed;
    /* Explicit native capability/admission, not inferred from a page name.
     * In particular SETUP must not silently forget an existing credential. */
    bool can_cancel;
    bool can_retry;
    bool can_setup;
    /* Initialization failure must remain visible even while unavailable
     * controls stay locked. This is not an association or command failure. */
    esp_err_t initialization_error;
    /* Command failure is distinct from the current native link state (e.g.
     * OFF rejected by OTA teardown while Wi-Fi remains genuinely online). */
    esp_err_t operation_error;
    char operation_label[16];
    esp_err_t error;
    int32_t failure_detail;
    char sta_ssid[RYZ_SYSTEM_UI_SSID_MAX_LENGTH + 1];
    char ipv4[RYZ_SYSTEM_UI_IPV4_MAX_LENGTH + 1];
    bool mac_valid;
    bool ipv6_valid;
    char mac[18];
    char ipv6[40];
    bool elapsed_valid;
    uint32_t elapsed_ms;
} ryz_v5_network_model_t;
int ryz_v5_network_info_height(const ryz_v5_network_model_t *model);

/* Draw original 240px V5 main content, excluding the shared y0..31 Header.
 * Root must be the current system candidate; caller owns create/commit and
 * revokes input on any error. No root/model pointer is retained. The one
 * dynamic AP QR image belongs to its LVGL object and is freed on deletion,
 * including candidate rollback. Widgets/fonts keep their existing lifecycle.
 * Local password uses a separate ephemeral Owner controller, never this
 * model. Forget/file-server backends remain unavailable; no sample values,
 * enabled mutation controls or file-server QR are emitted. */
esp_err_t ryz_v5_network_draw(lv_obj_t *root,
                              const ryz_v5_network_model_t *model);

/* Pure main-content hit test, not gesture recognition. The system owner must
 * enforce physical release, captured target, same-page generation and render
 * success before dispatching its result. Header back is handled by the owner.
 * A hit never means that a network command was admitted or completed. */
ryz_v5_network_action_t ryz_v5_network_hit(
    const ryz_v5_network_model_t *model, uint16_t x, uint16_t y);

const char *ryz_v5_network_title(ryz_v5_network_page_t page);
