#pragma once

#include "ryz_apps.h"
#include "ryz_ui_navigation.h"

/* Copy these identities from the frame actually shown. Both navigation and
 * reader identity are checked again by the UI Owner when consuming a tap. */
typedef struct {
    ryz_ui_nav_token_t page;
    uint64_t reader_request_id;
} ryz_workbench_apps_token_t;

typedef enum {
    RYZ_WB_APPS_PAGE,
    RYZ_WB_APPS_SELECT,
    RYZ_WB_APPS_BACK,
    RYZ_WB_APPS_RUN,
    RYZ_WB_APPS_DELETE,
} ryz_workbench_apps_action_t;

typedef struct {
    ryz_workbench_apps_token_t expected;
    ryz_workbench_apps_action_t action;
    size_t offset; /* PAGE only. */
    size_t limit;  /* PAGE only: 1..STORE_PAGE_MAX; renderer chooses geometry. */
    size_t index;  /* SELECT only: index within the displayed catalog page. */
    /* DELETE only, after explicit user confirmation. Copy the identity from
     * the displayed detail; never substitute a later selection's contents. */
    char delete_name[RYZ_SCRIPT_STORE_NAME_MAX + 1];
    char delete_sha256[65];
} ryz_workbench_apps_intent_t;

typedef enum {
    RYZ_WB_APPS_RUN_IDLE,
    RYZ_WB_APPS_RUN_PREPARING,
    RYZ_WB_APPS_RUN_STARTED,
    RYZ_WB_APPS_RUN_FAILED,
    RYZ_WB_APPS_RUN_UNKNOWN,
} ryz_workbench_apps_run_state_t;

typedef enum {
    RYZ_WB_APPS_DELETE_IDLE,
    RYZ_WB_APPS_DELETE_PENDING,
    RYZ_WB_APPS_DELETE_COMMITTED,
    RYZ_WB_APPS_DELETE_FAILED,
    RYZ_WB_APPS_DELETE_UNKNOWN,
} ryz_workbench_apps_delete_state_t;

typedef struct {
    uint64_t id;
    ryz_workbench_apps_delete_state_t state;
    char name[RYZ_SCRIPT_STORE_NAME_MAX + 1];
    esp_err_t error;
    /* Valid only when storage_result_valid. UNKNOWN never means a definite
     * failed deletion and must not be retried automatically. */
    bool storage_result_valid;
    ryz_script_store_commit_t commit;
    esp_err_t cleanup_error;
    bool recovery_required;
    char message[128];
} ryz_workbench_apps_delete_result_t;

typedef struct {
    bool open;
    ryz_workbench_apps_token_t token;
    ryz_apps_snapshot_t reader;
    size_t page_limit;
    ryz_workbench_apps_run_state_t run_state;
    uint64_t run_id;
    esp_err_t action_error;
    char message[128];
    char job_id[32];
    /* Latest confirmed delete, retained across navigation/close. A delete
     * admitted by RX can finish after leaving; navigation cannot roll it back. */
    ryz_workbench_apps_delete_result_t deletion;
} ryz_workbench_apps_snapshot_t;

/* Nonblocking, one pending intent. OK means copied, not already applied or
 * started. TIMEOUT means busy; INVALID_STATE means stale/closed/uninitialized.
 * No filesystem, JSON, Lua or device operation occurs in either public call.
 * snapshot clears non-NULL output on failure. Poll after accepted actions;
 * UNKNOWN must not be automatically retried or claimed as a failed launch. */
esp_err_t ryz_workbench_apps_submit(const ryz_workbench_apps_intent_t *intent);
esp_err_t ryz_workbench_apps_get_snapshot(ryz_workbench_apps_snapshot_t *out);
