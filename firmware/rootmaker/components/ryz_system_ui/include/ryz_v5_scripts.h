#pragma once

#include "lvgl.h"
#include "ryz_apps.h"

/* Pure C presentation of original V5 Scripts frames. The service owns page
 * generations, selection, filesystem transactions, and restart permission. */
typedef enum {
    RYZ_V5_SCRIPTS_SETTINGS, RYZ_V5_SCRIPTS_PICKER,
    RYZ_V5_SCRIPTS_SAVED, RYZ_V5_SCRIPTS_CLEAR_BOOT,
    RYZ_V5_SCRIPTS_DELETE_ALL = RYZ_V5_SCRIPTS_CLEAR_BOOT, /* Route ABI only. */
} ryz_v5_scripts_page_t;

typedef enum {
    RYZ_V5_SCRIPTS_IDLE, RYZ_V5_SCRIPTS_PENDING, RYZ_V5_SCRIPTS_COMMITTED,
    RYZ_V5_SCRIPTS_FAILED, RYZ_V5_SCRIPTS_UNKNOWN,
} ryz_v5_scripts_result_t;

typedef enum { RYZ_V5_SCRIPTS_OP_NONE, RYZ_V5_SCRIPTS_OP_BOOT,
               RYZ_V5_SCRIPTS_OP_CLEAR_BOOT,
               RYZ_V5_SCRIPTS_OP_DELETE_ALL = RYZ_V5_SCRIPTS_OP_CLEAR_BOOT } ryz_v5_scripts_operation_t;

typedef struct {
    uint64_t generation; /* Nonzero service view token; never reuse a stale generation. */
    ryz_v5_scripts_page_t page;
    bool boot_known, boot_bytes_known;
    char boot_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    size_t boot_bytes;
    uint32_t boot_revision;
    char boot_sha256[65]; /* Exact user boot.lua identity, even if Lua is invalid. */
    bool store_ready, recovery_required;
    ryz_apps_state_t catalog_state;
    ryz_script_store_page_t catalog;
    int scroll_y; /* Pixel offset in the full 31px-pitch Picker list. */
    /* A row highlight alone is not a version-bound selected source. */
    bool selected_identity_valid;
    bool selected_source_valid;
    esp_err_t selected_source_error;
    uint32_t selected_revision;
    ryz_script_store_description_t selected;
    bool can_open_picker, can_change_boot, can_clear_boot, can_restart;
    bool can_delete_all; /* Retained ABI field; never grants any permission. */
    bool show_saved; /* Successful copy completed on its still-current Picker. */
    ryz_v5_scripts_operation_t operation;
    ryz_v5_scripts_result_t result;
    esp_err_t error;
    size_t batch_total, batch_removed;
    bool batch_complete;
    esp_err_t cleanup_error;
    char failed_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    char result_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
} ryz_v5_scripts_snapshot_t;

typedef enum {
    RYZ_V5_SCRIPTS_ACTION_NONE, RYZ_V5_SCRIPTS_ACTION_BACK,
    RYZ_V5_SCRIPTS_ACTION_PICKER, RYZ_V5_SCRIPTS_ACTION_DELETE_CONFIRM,
    RYZ_V5_SCRIPTS_ACTION_SELECT, RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT,
    RYZ_V5_SCRIPTS_ACTION_CANCEL, RYZ_V5_SCRIPTS_ACTION_LATER,
    RYZ_V5_SCRIPTS_ACTION_RESTART, RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT,
    RYZ_V5_SCRIPTS_ACTION_DELETE_ALL = RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT,
    RYZ_V5_SCRIPTS_ACTION_PAGE,
} ryz_v5_scripts_action_t;

typedef struct {
    ryz_v5_scripts_action_t action;
    uint64_t generation;
    uint32_t revision;
    size_t index, offset, limit;
    int scroll_y;
    char name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
    char sha256[65];
} ryz_v5_scripts_intent_t;

typedef struct {
    esp_err_t (*submit)(void *context, const ryz_v5_scripts_intent_t *intent);
    void *context;
} ryz_v5_scripts_binding_t;
/* Owner-only binding. Submissions copy intents; no filesystem work on UI. */
void ryz_v5_scripts_bind(const ryz_v5_scripts_binding_t *binding);

/* The root supplies the shared y0..31 header. This component draws y32..239
 * (including the original 24px footer), never a bottom Home/Apps nav. */
esp_err_t ryz_v5_scripts_draw(lv_obj_t *root, const ryz_v5_scripts_snapshot_t *snapshot);
const char *ryz_v5_scripts_title(const ryz_v5_scripts_snapshot_t *snapshot);
/* Use only after a completed, uncancelled tap. A disabled control consumes
 * the hit but leaves action NONE. Non-NULL output is always cleared first.
 * This is an intent, never proof of a committed write. */
bool ryz_v5_scripts_hit(const ryz_v5_scripts_snapshot_t *snapshot,
                         uint16_t x, uint16_t y, ryz_v5_scripts_intent_t *out);
/* Gesture owner calls continuously after cancelling tap; delta_y is pixels,
 * positive advances down the list. Owner publishes scroll_y and fetches a new
 * bounded 16-entry window only when needed; no implicit IO here. */
bool ryz_v5_scripts_scroll(const ryz_v5_scripts_snapshot_t *snapshot,
                            int delta_y, ryz_v5_scripts_intent_t *out);
