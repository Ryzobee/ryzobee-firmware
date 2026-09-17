#include "workbench_apps.h"
#include "workbench_file_rpc.h"
#include "workbench_job_start.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

typedef struct {
    ryz_workbench_apps_token_t token;
    uint64_t id;
    bool deleting;
    char name[RYZ_SCRIPT_STORE_NAME_MAX + 1];
    char sha256[65];
} run_request_t;

typedef struct {
    run_request_t request;
    char *source;
    esp_err_t error;
    char message[128];
    ryz_workbench_apps_delete_result_t deletion;
} prepared_t;

enum { RUN_SLOT_IDLE, RUN_SLOT_REQUESTED, RUN_SLOT_WORKING, RUN_SLOT_READY };
static atomic_flag gate = ATOMIC_FLAG_INIT;
static atomic_bool initialized;
static char identity[24];
/* All shared storage is copied under gate; never hold it across FS, reader,
 * callbacks, JSON allocation or job admission. There is exactly one slot of
 * each kind, so a delayed RX/Owner cannot accumulate source buffers. */
/* Task-only bulk snapshots: cache-on access, with synchronization kept in
 * internal RAM. Preserve publication isolation; do not alias Owner and RX. */
static EXT_RAM_BSS_ATTR ryz_workbench_apps_snapshot_t published;
static bool has_intent;
static ryz_workbench_apps_intent_t pending_intent;
static unsigned run_slot;
static run_request_t pending_run;
static prepared_t prepared;
/* Owner-private state. RX never reads these fields. */
static EXT_RAM_BSS_ATTR ryz_workbench_apps_snapshot_t view;
static size_t saved_offset;
static size_t saved_limit = RYZ_SCRIPT_STORE_PAGE_MAX;
static bool back_fallback;
static uint64_t next_run;
static ryz_workbench_apps_delete_result_t last_deletion;
static ryz_workbench_apps_token_t last_delete_token;
/* RX-private pending publication, retained only on brief gate contention. */
static bool rx_has_result;
static prepared_t rx_result;

static bool take(void)
{ return !atomic_flag_test_and_set_explicit(&gate, memory_order_acquire); }
static void give(void)
{ atomic_flag_clear_explicit(&gate, memory_order_release); }

static bool same_page(ryz_ui_nav_token_t a, ryz_ui_nav_token_t b)
{ return a.generation == b.generation && a.route == b.route && a.modal == b.modal; }
static bool same_token(ryz_workbench_apps_token_t a, ryz_workbench_apps_token_t b)
{ return same_page(a.page, b.page) && a.reader_request_id == b.reader_request_id; }

static bool intent_matches(const ryz_workbench_apps_intent_t *intent,
                           ryz_workbench_apps_token_t token)
{
    return intent->action == RYZ_WB_APPS_BACK ?
        same_page(intent->expected.page, token.page) :
        same_token(intent->expected, token);
}

static bool valid_delete_identity(const ryz_workbench_apps_intent_t *intent)
{
    if (!memchr(intent->delete_name, 0, sizeof(intent->delete_name)) ||
        !ryz_script_store_valid_name(intent->delete_name) || intent->delete_sha256[64]) return false;
    for (size_t index = 0; index < 64; ++index) {
        const char c = intent->delete_sha256[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static void message(esp_err_t error, const char *text)
{
    view.action_error = error;
    snprintf(view.message, sizeof(view.message), "%s", text ? text : "");
}

esp_err_t ryz_workbench_apps_init(const char *boot_id)
{
    if (!boot_id || !boot_id[0] || strlen(boot_id) >= sizeof(identity))
        return ESP_ERR_INVALID_ARG;
    if (atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    strcpy(identity, boot_id);
    atomic_store(&initialized, true);
    return ESP_OK;
}

esp_err_t ryz_workbench_apps_get_snapshot(ryz_workbench_apps_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    if (!take()) return ESP_ERR_TIMEOUT;
    *out = published;
    give();
    return ESP_OK;
}

esp_err_t ryz_workbench_apps_submit(const ryz_workbench_apps_intent_t *intent)
{
    if (!intent || intent->action < RYZ_WB_APPS_PAGE || intent->action > RYZ_WB_APPS_DELETE ||
        !intent->expected.page.generation || !intent->expected.page.route || intent->expected.page.modal)
        return ESP_ERR_INVALID_ARG;
    if (intent->action == RYZ_WB_APPS_PAGE) {
        if (!intent->limit || intent->limit > RYZ_SCRIPT_STORE_PAGE_MAX ||
            intent->offset > RYZ_SCRIPT_STORE_INDEX_MAX || intent->index) return ESP_ERR_INVALID_ARG;
    } else if (intent->offset || intent->limit ||
               (intent->action != RYZ_WB_APPS_SELECT && intent->index)) return ESP_ERR_INVALID_ARG;
    if (intent->action == RYZ_WB_APPS_DELETE) {
        if (!valid_delete_identity(intent)) return ESP_ERR_INVALID_ARG;
    } else if (intent->delete_name[0] || intent->delete_sha256[0]) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    if (!take()) return ESP_ERR_TIMEOUT;
    esp_err_t error = !published.open || !intent_matches(intent, published.token)
        ? ESP_ERR_INVALID_STATE : has_intent ? ESP_ERR_TIMEOUT : ESP_OK;
    if (error == ESP_OK) { pending_intent = *intent; has_intent = true; }
    give();
    return error;
}

static void read_snapshot(void)
{
    /* view is Owner-private. Copy directly instead of putting another full
     * catalog+metadata snapshot on the embedded Owner's stack. */
    esp_err_t error = ryz_apps_get_snapshot(&view.reader);
    if (error != ESP_OK) {
        memset(&view.reader, 0, sizeof(view.reader));
        view.reader.state = RYZ_APPS_FAILED;
        view.reader.error = error;
    }
    view.token.reader_request_id = view.reader.request_id;
    if (view.reader.view == RYZ_APPS_VIEW_CATALOG &&
        (view.reader.state == RYZ_APPS_READY || view.reader.state == RYZ_APPS_EMPTY))
        saved_offset = view.reader.page.offset;
}

static void reset_run(void)
{
    view.run_state = RYZ_WB_APPS_RUN_IDLE;
    view.job_id[0] = 0;
    message(ESP_OK, "");
}

static void request_page(size_t offset, size_t limit, uint32_t revision)
{
    uint64_t request_id;
    esp_err_t error = ryz_apps_start();
    if (error == ESP_OK) error = ryz_apps_request_page(offset, limit, revision, &request_id);
    if (error != ESP_OK) message(error, "Application list unavailable; retry explicitly");
    else { saved_offset = offset; saved_limit = limit; view.page_limit = limit; }
    read_snapshot();
}

static void consume_intent(const ryz_workbench_apps_intent_t *intent)
{
    if (!intent_matches(intent, view.token)) {
        message(ESP_ERR_INVALID_STATE, "Page changed; use the current application view");
        return;
    }
    if ((intent->action == RYZ_WB_APPS_RUN || intent->action == RYZ_WB_APPS_DELETE) &&
        (last_deletion.state == RYZ_WB_APPS_DELETE_PENDING ||
         (last_deletion.state == RYZ_WB_APPS_DELETE_UNKNOWN &&
          same_token(view.token, last_delete_token)))) {
        message(ESP_ERR_INVALID_STATE, "Deletion pending or unknown; refresh before another action");
        return;
    }
    const ryz_apps_snapshot_t *reader = &view.reader;
    if (intent->action == RYZ_WB_APPS_PAGE) {
        uint32_t revision = reader->view == RYZ_APPS_VIEW_CATALOG &&
            (reader->state == RYZ_APPS_READY || reader->state == RYZ_APPS_EMPTY)
            ? reader->page.revision : 0;
        reset_run();
        back_fallback = false;
        request_page(intent->offset, intent->limit, revision);
    } else if (intent->action == RYZ_WB_APPS_SELECT) {
        if (reader->view != RYZ_APPS_VIEW_CATALOG || reader->state != RYZ_APPS_READY ||
            intent->index >= reader->page.count) {
            message(ESP_ERR_INVALID_STATE, "Application list changed or is not ready");
            return;
        }
        char name[RYZ_SCRIPT_STORE_NAME_MAX + 1];
        strcpy(name, reader->page.entries[intent->index].name);
        uint32_t revision = reader->page.revision;
        saved_offset = reader->page.offset;
        reset_run();
        uint64_t request_id;
        esp_err_t error = ryz_apps_request_detail(name, revision, &request_id);
        if (error != ESP_OK) message(error, "Application details unavailable");
        read_snapshot();
    } else if (intent->action == RYZ_WB_APPS_BACK) {
        if (reader->view != RYZ_APPS_VIEW_DETAIL) {
            message(ESP_ERR_INVALID_STATE, "No application detail to leave");
            return;
        }
        reset_run();
        back_fallback = saved_offset > 0;
        request_page(saved_offset, view.page_limit, 0);
    } else if (intent->action == RYZ_WB_APPS_DELETE) {
        if (view.run_state == RYZ_WB_APPS_RUN_PREPARING ||
            view.run_state == RYZ_WB_APPS_RUN_STARTED || view.run_state == RYZ_WB_APPS_RUN_UNKNOWN) {
            message(ESP_ERR_INVALID_STATE, "Application launch already pending or accepted");
            return;
        }
        if (next_run == UINT64_MAX) {
            message(ESP_ERR_INVALID_STATE, "Application operation identity exhausted");
            return;
        }
        esp_err_t error = ESP_OK;
        const char *text = "Deleting confirmed application";
        if (reader->view != RYZ_APPS_VIEW_DETAIL || reader->state != RYZ_APPS_READY ||
            !reader->store_ready || reader->recovery_required ||
            strcmp(reader->detail.file.entry.name, intent->delete_name) ||
            strcmp(reader->detail.file.sha256, intent->delete_sha256)) {
            error = ESP_ERR_INVALID_STATE;
            text = "Confirmed application changed; select it again";
        } else if (reader->detail.file.entry.protected_file) {
            error = ESP_ERR_NOT_SUPPORTED;
            text = "Protected application cannot be deleted";
        }
        if (!take()) { message(ESP_ERR_TIMEOUT, "Application operation queue busy"); return; }
        if (run_slot != RUN_SLOT_IDLE) {
            give();
            message(ESP_ERR_TIMEOUT, "Previous application operation is still finishing");
            return;
        }
        last_deletion = (ryz_workbench_apps_delete_result_t){.id = ++next_run,
            .state = error == ESP_OK ? RYZ_WB_APPS_DELETE_PENDING : RYZ_WB_APPS_DELETE_FAILED,
            .error = error};
        strcpy(last_deletion.name, intent->delete_name);
        snprintf(last_deletion.message, sizeof(last_deletion.message), "%s", text);
        last_delete_token = view.token;
        if (error == ESP_OK) {
            pending_run = (run_request_t){.token = view.token, .id = last_deletion.id, .deleting = true};
            strcpy(pending_run.name, intent->delete_name);
            strcpy(pending_run.sha256, intent->delete_sha256);
            run_slot = RUN_SLOT_REQUESTED;
        }
        give();
        message(error, text);
    } else {
        if (view.run_state == RYZ_WB_APPS_RUN_PREPARING ||
            view.run_state == RYZ_WB_APPS_RUN_STARTED || view.run_state == RYZ_WB_APPS_RUN_UNKNOWN) {
            message(ESP_ERR_INVALID_STATE, "Application launch already pending or accepted");
            return;
        }
        if (reader->view != RYZ_APPS_VIEW_DETAIL || reader->state != RYZ_APPS_READY ||
            !reader->store_ready || reader->recovery_required || !reader->detail.source_valid) {
            message(ESP_ERR_INVALID_STATE, "Application is invalid, changed, or storage is unavailable");
            return;
        }
        if (next_run == UINT64_MAX) {
            message(ESP_ERR_INVALID_STATE, "Application launch identity exhausted");
            return;
        }
        if (!take()) { message(ESP_ERR_TIMEOUT, "Application launch queue busy"); return; }
        if (run_slot != RUN_SLOT_IDLE) {
            give();
            message(ESP_ERR_TIMEOUT, "Previous application read is still finishing");
            return;
        }
        pending_run = (run_request_t){ .token = view.token, .id = ++next_run };
        strcpy(pending_run.name, reader->detail.file.entry.name);
        strcpy(pending_run.sha256, reader->detail.file.sha256);
        run_slot = RUN_SLOT_REQUESTED;
        view.run_id = pending_run.id;
        give();
        view.run_state = RYZ_WB_APPS_RUN_PREPARING;
        message(ESP_OK, "Preparing selected application");
    }
}

void ryz_workbench_apps_owner_tick(bool active, ryz_ui_nav_token_t page,
                                  ryz_workbench_apps_launch_fn launch, void *context)
{
    if (!atomic_load(&initialized)) return;
    bool intent_ready = false, result_ready = false;
    ryz_workbench_apps_intent_t intent;
    prepared_t result = {0};
    if (take()) {
        if (has_intent) { intent = pending_intent; has_intent = false; intent_ready = true; }
        if (run_slot == RUN_SLOT_READY) {
            result = prepared; memset(&prepared, 0, sizeof(prepared));
            run_slot = RUN_SLOT_IDLE; result_ready = true;
        }
        give();
    }
    if (!active || !page.generation || !page.route || page.modal) {
        if (view.open) (void)ryz_apps_close();
        memset(&view, 0, sizeof(view));
        back_fallback = false;
    } else {
        if (!view.open || !same_page(page, view.token.page)) {
            if (view.open) (void)ryz_apps_close();
            memset(&view, 0, sizeof(view));
            view.open = true; view.token.page = page;
            view.page_limit = saved_limit;
            back_fallback = saved_offset > 0;
            request_page(saved_offset, view.page_limit, 0);
        } else read_snapshot();
        if (back_fallback && view.reader.view == RYZ_APPS_VIEW_CATALOG &&
            view.reader.state != RYZ_APPS_LOADING) {
            back_fallback = false;
            if (view.reader.state == RYZ_APPS_FAILED && view.reader.error == ESP_ERR_INVALID_ARG)
                request_page(0, view.page_limit, 0);
        }
        /* A BACK/new selection queued before this tick wins over a prepared
         * launch result delivered in the same tick. No callback runs stale. */
        if (intent_ready) consume_intent(&intent);
        if (result_ready && !result.request.deleting &&
            view.run_state == RYZ_WB_APPS_RUN_PREPARING && view.run_id == result.request.id) {
            bool current = same_token(view.token, result.request.token) &&
                view.reader.view == RYZ_APPS_VIEW_DETAIL && view.reader.state == RYZ_APPS_READY &&
                view.reader.store_ready && !view.reader.recovery_required &&
                view.reader.detail.source_valid &&
                !strcmp(view.reader.detail.file.entry.name, result.request.name) &&
                !strcmp(view.reader.detail.file.sha256, result.request.sha256);
            view.run_state = RYZ_WB_APPS_RUN_FAILED;
            if (!current) message(ESP_ERR_INVALID_STATE, "Selected application changed; select it again");
            else if (!result.source) message(result.error, result.message);
            else if (!launch) message(ESP_ERR_NOT_SUPPORTED, "Application execution unavailable");
            else {
                ryz_workbench_apps_launch_result_t admitted = {0};
                char *source = result.source; result.source = NULL;
                launch(context, result.request.name, source, &admitted);
                view.run_state = admitted.uncertain ? RYZ_WB_APPS_RUN_UNKNOWN : admitted.accepted
                    ? RYZ_WB_APPS_RUN_STARTED : RYZ_WB_APPS_RUN_FAILED;
                message(admitted.error, admitted.message);
                snprintf(view.job_id, sizeof(view.job_id), "%s", admitted.job_id);
            }
        }
    }
    if (result_ready && result.request.deleting && last_deletion.id == result.request.id) {
        last_deletion = result.deletion;
        /* A committed delete remains observable even if the caller left. It
         * must not navigate a different page or silently retry an unknown. */
        if (view.open && same_token(view.token, result.request.token)) {
            message(last_deletion.error, last_deletion.message);
            if (last_deletion.state == RYZ_WB_APPS_DELETE_COMMITTED) {
                reset_run();
                back_fallback = saved_offset > 0;
                request_page(saved_offset, view.page_limit, 0);
                message(last_deletion.error, last_deletion.message);
            }
        }
    }
    free(result.source);
    view.deletion = last_deletion;
    if (take()) { published = view; give(); }
}

static cJSON *prepare_source(void *context, const char *id,
                             const ryz_script_store_snapshot_t *snapshot)
{
    (void)id;
    prepared_t *out = context;
    out->source = ryz_workbench_source_copy(snapshot->source, snapshot->entry.bytes);
    if (!out->source) {
        out->error = ESP_ERR_NO_MEM;
        snprintf(out->message, sizeof(out->message), "Cannot allocate application source");
        return NULL;
    }
    out->error = ESP_OK;
    /* Internal-only use of the real version-bound loader: bytes are adopted,
     * but no job is published here. The trusted context records preparation;
     * NULL is never serialized as a launch ACK or interpreted as permission
     * to retry. The Owner performs the final admission from these exact bytes. */
    return NULL;
}

static void delete_reply(prepared_t *out, const cJSON *reply)
{
    ryz_workbench_apps_delete_result_t *value = &out->deletion;
    value->state = RYZ_WB_APPS_DELETE_UNKNOWN;
    value->error = reply ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NO_MEM;
    snprintf(value->message, sizeof(value->message), "Deletion result unknown; refresh and inspect storage");
    if (!cJSON_IsObject(reply)) return;
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(reply, "ok");
    const cJSON *commit = cJSON_GetObjectItemCaseSensitive(reply, "store_commit");
    const cJSON *cleanup = cJSON_GetObjectItemCaseSensitive(reply, "store_cleanup_error");
    const cJSON *recovery = cJSON_GetObjectItemCaseSensitive(reply, "store_recovery_required");
    if (!cJSON_IsBool(ok)) return;
    if (cJSON_IsString(commit) && cJSON_IsNumber(cleanup) &&
        cleanup->valuedouble == cleanup->valueint && cJSON_IsBool(recovery)) {
        if (!strcmp(commit->valuestring, "committed") && cJSON_IsTrue(ok)) {
            value->state = RYZ_WB_APPS_DELETE_COMMITTED;
            value->commit = RYZ_SCRIPT_STORE_COMMITTED;
        } else if (!strcmp(commit->valuestring, "not_committed") && cJSON_IsFalse(ok)) {
            value->state = RYZ_WB_APPS_DELETE_FAILED;
            value->commit = RYZ_SCRIPT_STORE_NOT_COMMITTED;
        } else if (!strcmp(commit->valuestring, "unknown") && cJSON_IsFalse(ok)) {
            value->commit = RYZ_SCRIPT_STORE_COMMIT_UNKNOWN;
        } else return;
        value->storage_result_valid = true;
        value->cleanup_error = cleanup->valueint;
        value->recovery_required = cJSON_IsTrue(recovery);
        value->error = value->state == RYZ_WB_APPS_DELETE_COMMITTED ? value->cleanup_error : ESP_FAIL;
        if (value->state == RYZ_WB_APPS_DELETE_COMMITTED) {
            snprintf(value->message, sizeof(value->message), "%s",
                value->cleanup_error != ESP_OK || value->recovery_required
                ? "File deleted; storage recovery required" : "Application deleted");
            return;
        }
    } else if (cJSON_IsFalse(ok) && !commit) {
        value->state = RYZ_WB_APPS_DELETE_FAILED;
        value->error = ESP_ERR_INVALID_STATE;
    } else return;
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
    if (cJSON_IsString(error)) snprintf(value->message, sizeof(value->message), "%s", error->valuestring);
}

void ryz_workbench_apps_rx_tick(bool runtime_busy,
                               const ryz_workbench_write_guard_t *guard)
{
    if (!atomic_load(&initialized)) return;
    if (!rx_has_result) {
        if (!take()) return;
        if (run_slot != RUN_SLOT_REQUESTED) { give(); return; }
        rx_result = (prepared_t){ .request = pending_run, .error = ESP_ERR_NO_MEM };
        const bool deleting = pending_run.deleting;
        const bool newer_navigation = has_intent &&
            (pending_intent.action == RYZ_WB_APPS_PAGE || pending_intent.action == RYZ_WB_APPS_SELECT ||
             pending_intent.action == RYZ_WB_APPS_BACK);
        /* DELETE's acceptance point: after this short gate, confirmed CAS
         * may complete even if navigation changes while Store is executing. */
        const bool may_delete = !runtime_busy && !newer_navigation && published.open &&
            same_token(published.token, pending_run.token) &&
            published.reader.view == RYZ_APPS_VIEW_DETAIL && published.reader.state == RYZ_APPS_READY &&
            published.reader.store_ready && !published.reader.recovery_required &&
            !published.reader.detail.file.entry.protected_file &&
            !strcmp(published.reader.detail.file.entry.name, pending_run.name) &&
            !strcmp(published.reader.detail.file.sha256, pending_run.sha256);
        run_slot = RUN_SLOT_WORKING;
        give();
        if (deleting) {
            rx_result.deletion = (ryz_workbench_apps_delete_result_t){.id = rx_result.request.id,
                .state = RYZ_WB_APPS_DELETE_FAILED, .error = ESP_ERR_NO_MEM};
            strcpy(rx_result.deletion.name, rx_result.request.name);
            snprintf(rx_result.deletion.message, sizeof(rx_result.deletion.message),
                     "Cannot allocate deletion request; file unchanged");
        }
        snprintf(rx_result.message, sizeof(rx_result.message), "Cannot prepare application request");
        cJSON *request = deleting && !may_delete ? NULL : cJSON_CreateObject();
        bool valid = request && cJSON_AddStringToObject(request, "id", "system-apps") &&
            cJSON_AddStringToObject(request, "op", "scripts") &&
            cJSON_AddStringToObject(request, "schema", "ryz-script-store/1") &&
            cJSON_AddStringToObject(request, "boot_id", identity) &&
            cJSON_AddStringToObject(request, "action", deleting ? "remove" : "run") &&
            cJSON_AddStringToObject(request, "name", rx_result.request.name) &&
            cJSON_AddStringToObject(request, deleting ? "previous_sha256" : "sha256", rx_result.request.sha256);
        if (valid) {
            cJSON *reply = ryz_workbench_file_rpc(request, identity, runtime_busy,
                                                 prepare_source, &rx_result, guard);
            if (deleting) delete_reply(&rx_result, reply);
            else if (reply) {
                const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
                rx_result.error = ESP_ERR_INVALID_STATE;
                snprintf(rx_result.message, sizeof(rx_result.message), "%s",
                         cJSON_IsString(error) ? error->valuestring : "Application preparation failed");
            }
            cJSON_Delete(reply);
        }
        if (deleting && !may_delete) {
            rx_result.deletion.error = runtime_busy ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
            snprintf(rx_result.deletion.message, sizeof(rx_result.deletion.message), "%s",
                     runtime_busy ? "Runtime busy; file unchanged" : "Application page changed; file unchanged");
        }
        cJSON_Delete(request);
        rx_has_result = true;
    }
    if (take()) {
        prepared = rx_result;
        memset(&rx_result, 0, sizeof(rx_result));
        rx_has_result = false;
        run_slot = RUN_SLOT_READY;
        give();
    }
}
