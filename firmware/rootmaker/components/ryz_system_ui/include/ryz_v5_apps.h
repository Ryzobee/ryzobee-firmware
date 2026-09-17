#pragma once

#include "lvgl.h"
#include "ryz_apps.h"
#include "ryz_ui_navigation.h"
#include "touch_protocol.h"

/* Trusted Owner-only UI boundary. The implementation never reads files or
 * starts jobs: get is copy-only; submit copies a version-bound intent. */
typedef struct {
    ryz_ui_nav_token_t page;
    uint64_t reader_request_id;
} ryz_v5_apps_token_t;

typedef enum {
    RYZ_V5_APPS_PAGE, RYZ_V5_APPS_SELECT, RYZ_V5_APPS_BACK,
    RYZ_V5_APPS_RUN, RYZ_V5_APPS_DELETE,
} ryz_v5_apps_action_t;

typedef struct {
    ryz_v5_apps_token_t expected;
    ryz_v5_apps_action_t action;
    size_t offset, limit, index;
    char delete_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    char delete_sha256[65];
} ryz_v5_apps_intent_t;

typedef enum {
    RYZ_V5_APPS_IDLE, RYZ_V5_APPS_PREPARING, RYZ_V5_APPS_STARTED,
    RYZ_V5_APPS_FAILED, RYZ_V5_APPS_UNKNOWN,
} ryz_v5_apps_run_state_t;

typedef enum {
    RYZ_V5_APPS_DELETE_IDLE, RYZ_V5_APPS_DELETE_PENDING,
    RYZ_V5_APPS_DELETE_COMMITTED, RYZ_V5_APPS_DELETE_FAILED,
    RYZ_V5_APPS_DELETE_UNKNOWN,
} ryz_v5_apps_delete_state_t;

typedef struct {
    bool open;
    ryz_v5_apps_token_t token;
    ryz_apps_snapshot_t reader;
    size_t page_limit;
    ryz_v5_apps_run_state_t run_state;
    ryz_v5_apps_delete_state_t delete_state;
    bool delete_recovery_required;
    esp_err_t delete_error, delete_cleanup_error;
    esp_err_t action_error;
    char message[128];
} ryz_v5_apps_snapshot_t;

typedef struct {
    esp_err_t (*get)(void *context, ryz_v5_apps_snapshot_t *out);
    esp_err_t (*submit)(void *context, const ryz_v5_apps_intent_t *intent);
    void *context;
} ryz_v5_apps_binding_t;

/* Bind before the Owner starts. NULL unbinds. reset preserves the binding and
 * bounded list/pixel anchor, returns to the APPS stream root, revokes gestures/dialogs and
 * requires a physical release. Late reader data cannot open a child page. */
void ryz_v5_apps_bind(const ryz_v5_apps_binding_t *binding);
void ryz_v5_apps_reset(void);
/* Revoke the current contact (e.g. the shell acquired horizontal paging or
 * input was lost), without discarding the current page or scroll position. */
void ryz_v5_apps_cancel_gesture(void);
/* Owner-only wheel/accessibility input. Positive pixels browse downward.
 * Cancels any pending tap/hold; never selects or executes a file. */
void ryz_v5_apps_scroll(int delta_y);
/* Poll only while the Apps route is interactive. True means repaint needed;
 * it is not a promise that data or a run is ready. No LVGL work in tick. */
bool ryz_v5_apps_tick(uint64_t now_ms);
/* Root calls after its own release guard; cancel_gesture handles input loss
 * or horizontal takeover, reset handles route changes. True consumes the sample.
 * Header back is handled here in browser/detail/dialog; the stream root
 * has a non-interactive brand plate instead of Back. */
bool ryz_v5_apps_pointer(const ryz_touch_sample_t *sample, uint64_t now_ms);
/* Pure render of cached state into the current system candidate. Header is
 * y0..31; list has bottom navigation y196..239, detail/dialog do not.
 * RUN widget references are revoked by root deletion; caller owns the lease. */
esp_err_t ryz_v5_apps_draw(lv_obj_t *root);
/* Owner-only in-place RUN feedback on the committed root. False requires a
 * full draw (reader/content/scroll/dialog changed). Caller must pump LVGL and
 * revoke action eligibility on any cancelled/failed transfer, as with commit. */
bool ryz_v5_apps_update(lv_obj_t *root);
const char *ryz_v5_apps_title(void);
bool ryz_v5_apps_has_navigation(void);
bool ryz_v5_apps_has_back(void);
