#include "workbench_scripts.h"
#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

static atomic_flag gate = ATOMIC_FLAG_INIT;
static atomic_bool initialized;
/* Owner revokes an old published view before any external cache operation.
 * A failed publication try-lock cannot leave its old confirmation admissible. */
static atomic_bool live_view;
/* Owner/RX task snapshots require the cache. Only their bulk data lives in
 * PSRAM; admission guards and lifecycle state remain in internal RAM. */
static EXT_RAM_BSS_ATTR ryz_v5_scripts_snapshot_t published, view, last_model;
static bool published_open, open;
static ryz_ui_nav_token_t navigation;
static uint64_t next_generation;
static EXT_RAM_BSS_ATTR ryz_apps_snapshot_t reader;
static EXT_RAM_BSS_ATTR ryz_script_store_page_t catalog;
static bool has_intent;
static ryz_v5_scripts_intent_t pending_intent;
static uint64_t selected_request;
static char selected_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
typedef enum { SLOT_IDLE, SLOT_REQUESTED, SLOT_WORKING, SLOT_READY } slot_state_t;
typedef struct {
    uint64_t session, generation, id;
    uint32_t revision;
    ryz_v5_scripts_operation_t operation;
    char name[RYZ_SCRIPT_STORE_NAME_MAX + 1U], sha256[65];
} request_t;
typedef struct {
    request_t request;
    esp_err_t error;
    bool boot_exists;
    ryz_script_store_description_t boot;
    ryz_script_store_mutation_t mutation;
} result_t;
/* One shared request/result slot. RX retains a private completed result if a
 * short publication attempt is busy; it never repeats the external operation. */
static slot_state_t slot;
static request_t request;
static result_t result, rx_result;
static bool rx_has_result;
static uint64_t session, next_session, published_session;
static uint32_t boot_attempt_revision;
static unsigned boot_attempts, boot_retry_ticks;
static uint64_t next_write;
static bool queue_write;
static ryz_ui_nav_token_t queued_navigation;
static result_t last_write;
static ryz_v5_scripts_result_t last_write_state;

static bool take(void)
{ return !atomic_flag_test_and_set_explicit(&gate, memory_order_acquire); }
static void give(void) { atomic_flag_clear_explicit(&gate, memory_order_release); }
static bool same_nav(ryz_ui_nav_token_t a, ryz_ui_nav_token_t b)
{ return a.generation == b.generation && a.route == b.route && a.modal == b.modal; }

static void update_generation(void)
{
    if (!open) view.generation = 0;
    else if (memcmp(&view, &last_model, sizeof(view))) {
        view.generation = next_generation == UINT64_MAX ? 0 : ++next_generation;
    }
    if (view.generation != last_model.generation) atomic_store(&live_view, false);
    last_model = view;
}

static bool ready_catalog(void)
{ return view.store_ready && !view.recovery_required && catalog.revision &&
    (view.catalog_state == RYZ_APPS_READY || view.catalog_state == RYZ_APPS_EMPTY); }

static bool valid_sha(const char *hash)
{
    if (hash[64]) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f'))) return false;
    return true;
}

static void copy_write_result(void)
{
    if (!last_write.request.id) return;
    view.operation = last_write.request.operation;
    view.result = last_write_state;
    view.error = last_write.error;
    view.cleanup_error = last_write.mutation.cleanup_error;
    if (last_write.mutation.recovery_required) view.recovery_required = true;
    strcpy(view.result_name, last_write.request.name);
}

static void permissions(void)
{
    const bool idle = view.result == RYZ_V5_SCRIPTS_IDLE ||
        (view.page == RYZ_V5_SCRIPTS_SETTINGS &&
         (view.result == RYZ_V5_SCRIPTS_COMMITTED || view.result == RYZ_V5_SCRIPTS_FAILED));
    view.can_open_picker = open && ready_catalog() && view.result != RYZ_V5_SCRIPTS_PENDING &&
        view.result != RYZ_V5_SCRIPTS_UNKNOWN;
    view.can_change_boot = open && ready_catalog() && view.page == RYZ_V5_SCRIPTS_PICKER &&
        view.result == RYZ_V5_SCRIPTS_IDLE && view.selected_identity_valid &&
        view.selected_source_valid && view.selected_revision == catalog.revision &&
        valid_sha(view.selected.sha256);
    view.can_restart = view.can_delete_all = false;
    view.can_clear_boot = open && ready_catalog() && idle && view.boot_known &&
        !strcmp(view.boot_name, "boot.lua") && valid_sha(view.boot_sha256) &&
        view.boot_revision == catalog.revision && view.catalog.revision == catalog.revision &&
        (view.page == RYZ_V5_SCRIPTS_SETTINGS || view.page == RYZ_V5_SCRIPTS_CLEAR_BOOT);
}

static void clear_boot(void)
{
    view.boot_known = view.boot_bytes_known = false;
    view.boot_name[0] = 0; view.boot_bytes = 0;
    view.boot_revision = 0; memset(view.boot_sha256, 0, sizeof(view.boot_sha256));
}

static void clear_selection(void)
{
    view.selected_identity_valid = view.selected_source_valid = false;
    view.selected_source_error = ESP_OK;
    view.selected_revision = 0;
    memset(&view.selected, 0, sizeof(view.selected));
    selected_request = 0; selected_name[0] = 0;
}

static void request_page(size_t offset, uint32_t revision)
{
    uint64_t id;
    esp_err_t error = ryz_apps_start();
    if (error == ESP_OK) error = ryz_apps_request_page(offset, RYZ_SCRIPT_STORE_PAGE_MAX, revision, &id);
    view.catalog_state = error == ESP_OK ? RYZ_APPS_LOADING : RYZ_APPS_FAILED;
    view.error = error;
}

static void read_reader(void)
{
    esp_err_t error = ryz_apps_get_snapshot(&reader);
    if (error != ESP_OK) {
        view.catalog_state = RYZ_APPS_FAILED; view.error = error;
        view.store_ready = false; clear_selection(); clear_boot(); return;
    }
    if (reader.store_status_valid) {
        view.store_ready = reader.store_ready;
        view.recovery_required = reader.recovery_required;
    }
    if ((reader.store_status_valid && (!reader.store_ready || reader.recovery_required)) ||
        reader.state == RYZ_APPS_FAILED || reader.state == RYZ_APPS_STALE) {
        view.catalog_state = reader.state == RYZ_APPS_STALE ? RYZ_APPS_STALE : RYZ_APPS_FAILED;
        view.error = reader.error != ESP_OK ? reader.error : ESP_ERR_INVALID_STATE;
        clear_selection(); clear_boot(); return;
    }
    if (reader.view == RYZ_APPS_VIEW_CATALOG) {
        view.catalog_state = reader.state;
        if (reader.state == RYZ_APPS_READY || reader.state == RYZ_APPS_EMPTY) {
            if (!reader.page.revision || reader.page.count > RYZ_SCRIPT_STORE_PAGE_MAX ||
                reader.page.total > RYZ_SCRIPT_STORE_INDEX_MAX ||
                reader.page.offset > reader.page.total ||
                reader.page.count > reader.page.total - reader.page.offset) {
                view.catalog_state = RYZ_APPS_FAILED;
                view.error = ESP_ERR_INVALID_STATE; clear_selection(); clear_boot(); return;
            }
            if (catalog.revision != reader.page.revision) {
                clear_selection();
                clear_boot();
            }
            catalog = reader.page;
            int maximum = (int)catalog.total * 31 - 1 - 134;
            if (maximum < 0) maximum = 0;
            if (view.scroll_y > maximum) view.scroll_y = maximum;
        }
    } else if (reader.view == RYZ_APPS_VIEW_DETAIL && selected_request &&
               reader.request_id == selected_request && reader.state == RYZ_APPS_READY) {
        if (reader.completed_id != selected_request || reader.detail.revision != catalog.revision ||
            strcmp(reader.detail.file.entry.name, selected_name)) {
            clear_selection(); view.catalog_state = RYZ_APPS_STALE; return;
        }
        view.selected_identity_valid = true;
        view.selected_source_valid = reader.detail.source_valid;
        view.selected_source_error = reader.detail.source_error;
        view.selected_revision = reader.detail.revision;
        view.selected = reader.detail.file;
    }
    if (view.page != RYZ_V5_SCRIPTS_DELETE_ALL) view.catalog = catalog;
}

esp_err_t ryz_workbench_scripts_init(void)
{
    if (atomic_exchange(&initialized, true)) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
esp_err_t ryz_workbench_scripts_get_snapshot(ryz_v5_scripts_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    if (!take()) return ESP_ERR_TIMEOUT;
    *out = published;
    give();
    return ESP_OK;
}
esp_err_t ryz_workbench_scripts_submit(const ryz_v5_scripts_intent_t *intent)
{
    if (!intent) return ESP_ERR_INVALID_ARG;
    if (intent->action == RYZ_V5_SCRIPTS_ACTION_RESTART) return ESP_ERR_NOT_SUPPORTED;
    if (!intent->generation || !memchr(intent->name, 0, sizeof(intent->name)) ||
        !memchr(intent->sha256, 0, sizeof(intent->sha256))) return ESP_ERR_INVALID_ARG;
    if (intent->action == RYZ_V5_SCRIPTS_ACTION_PAGE) {
        if (intent->index || intent->name[0] || intent->sha256[0] ||
            intent->limit != RYZ_SCRIPT_STORE_PAGE_MAX || intent->offset > RYZ_SCRIPT_STORE_INDEX_MAX ||
            intent->scroll_y < 0 || intent->scroll_y > (int)RYZ_SCRIPT_STORE_INDEX_MAX * 31)
            return ESP_ERR_INVALID_ARG;
    } else if (intent->action == RYZ_V5_SCRIPTS_ACTION_SELECT) {
        if (intent->offset || intent->limit || intent->scroll_y || intent->sha256[0] ||
            !ryz_script_store_valid_name(intent->name)) return ESP_ERR_INVALID_ARG;
    } else if (intent->action == RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT ||
               intent->action == RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT) {
        if (intent->index || intent->offset || intent->limit || intent->scroll_y ||
            !valid_sha(intent->sha256) || !ryz_script_store_valid_name(intent->name) ||
            (intent->action == RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT && strcmp(intent->name, "boot.lua")))
            return ESP_ERR_INVALID_ARG;
    } else return ESP_ERR_NOT_SUPPORTED;
    if (!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    if (!take()) return ESP_ERR_TIMEOUT;
    esp_err_t error = !atomic_load(&live_view) || !published_open || published.generation != intent->generation ||
        published.catalog.revision != intent->revision ? ESP_ERR_INVALID_STATE :
        has_intent ? ESP_ERR_TIMEOUT : ESP_OK;
    if (error == ESP_OK) {
        if (intent->action == RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT) {
            if (published.page != RYZ_V5_SCRIPTS_CLEAR_BOOT || !published.can_clear_boot ||
                strcmp(intent->sha256, published.boot_sha256)) error = ESP_ERR_INVALID_STATE;
        } else if (published.page != RYZ_V5_SCRIPTS_PICKER) error = ESP_ERR_INVALID_STATE;
        else if (intent->action == RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT &&
                 (!published.can_change_boot || strcmp(intent->name, published.selected.entry.name) ||
                  strcmp(intent->sha256, published.selected.sha256))) error = ESP_ERR_INVALID_STATE;
        else if ((intent->action == RYZ_V5_SCRIPTS_ACTION_PAGE || intent->action == RYZ_V5_SCRIPTS_ACTION_SELECT) &&
                 (published.result == RYZ_V5_SCRIPTS_PENDING || published.result == RYZ_V5_SCRIPTS_UNKNOWN))
            error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) { pending_intent = *intent; has_intent = true; }
    give(); return error;
}

static void consume_intent(const ryz_v5_scripts_intent_t *intent)
{
    if (intent->generation != view.generation || intent->revision != view.catalog.revision) return;
    if (intent->action == RYZ_V5_SCRIPTS_ACTION_PAGE && view.page == RYZ_V5_SCRIPTS_PICKER) {
        if (!ready_catalog()) return;
        int maximum = (int)catalog.total * 31 - 1 - 134;
        if (maximum < 0) maximum = 0;
        if (intent->scroll_y > maximum || intent->offset > catalog.total) return;
        size_t first = (size_t)intent->scroll_y / 31;
        size_t end = (size_t)(intent->scroll_y + 134 + 30) / 31;
        if (end > catalog.total) end = catalog.total;
        if (first < intent->offset || end > intent->offset + RYZ_SCRIPT_STORE_PAGE_MAX) return;
        view.scroll_y = intent->scroll_y;
        if (intent->offset != catalog.offset || first < catalog.offset || end > catalog.offset + catalog.count)
            request_page(intent->offset, intent->revision);
    } else if (intent->action == RYZ_V5_SCRIPTS_ACTION_SELECT && view.page == RYZ_V5_SCRIPTS_PICKER) {
        if (!ready_catalog() || intent->index >= catalog.count ||
            strcmp(intent->name, catalog.entries[intent->index].name)) {
            view.error = ESP_ERR_INVALID_STATE; return;
        }
        clear_selection();
        /* A fresh user selection is an explicit retry opportunity after a
         * known failure; it never retries a write or clears an unknown result. */
        if (view.result == RYZ_V5_SCRIPTS_FAILED) {
            view.operation = RYZ_V5_SCRIPTS_OP_NONE;
            view.result = RYZ_V5_SCRIPTS_IDLE;
        }
        strcpy(selected_name, intent->name);
        view.error = ryz_apps_request_detail(selected_name, catalog.revision, &selected_request);
        if (view.error != ESP_OK) clear_selection();
    } else if ((intent->action == RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT &&
                view.page == RYZ_V5_SCRIPTS_CLEAR_BOOT && view.can_clear_boot) ||
               (intent->action == RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT &&
                view.page == RYZ_V5_SCRIPTS_PICKER && view.can_change_boot)) {
        if (next_write == UINT64_MAX) { view.result = RYZ_V5_SCRIPTS_FAILED; view.error = ESP_ERR_INVALID_STATE; return; }
        last_write = (result_t){.request = {.session = session, .id = ++next_write,
            .revision = intent->revision, .operation = intent->action == RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT ?
                RYZ_V5_SCRIPTS_OP_CLEAR_BOOT : RYZ_V5_SCRIPTS_OP_BOOT}};
        strcpy(last_write.request.name, intent->name);
        strcpy(last_write.request.sha256, intent->sha256);
        last_write_state = RYZ_V5_SCRIPTS_PENDING;
        queued_navigation = navigation;
        queue_write = true;
        view.show_saved = false;
        copy_write_result();
    }
}
void ryz_workbench_scripts_owner_tick(bool active, ryz_ui_nav_token_t token, ryz_v5_scripts_page_t page)
{
    if (!atomic_load(&initialized)) return;
    active = active && token.generation && token.route && page >= RYZ_V5_SCRIPTS_SETTINGS &&
        page <= RYZ_V5_SCRIPTS_DELETE_ALL;
    const bool changed_page = !active || !open || !same_nav(navigation, token) || view.page != page;
    if (changed_page) { atomic_store(&live_view, false); view.show_saved = false; }
    if (boot_retry_ticks) --boot_retry_ticks;
    ryz_v5_scripts_intent_t intent;
    bool got_intent = false;
    result_t ready = {0};
    bool got_result = false;
    if (take()) {
        if (has_intent) { intent = pending_intent; has_intent = false; got_intent = true; }
        if (slot == SLOT_READY) { ready = result; slot = SLOT_IDLE; got_result = true; }
        give();
    }
    if (!active) {
        if (open) (void)ryz_apps_close();
        open = false;
        memset(&view, 0, sizeof(view));
        memset(&catalog, 0, sizeof(catalog));
        clear_selection();
        copy_write_result();
    } else {
        if (!open) {
            if (next_session == UINT64_MAX) return;
            session = ++next_session;
            boot_attempt_revision = 0;
            boot_attempts = boot_retry_ticks = 0;
            open = true;
            memset(&view, 0, sizeof(view));
            copy_write_result();
            view.page = page;
            request_page(0, 0);
        }
        if (!same_nav(navigation, token)) {
            navigation = token; view.generation = 0;
            /* A deliberate re-entry is a read-only retry, never a write retry. */
            if (!view.boot_known) boot_attempts = boot_retry_ticks = 0;
            if (page == RYZ_V5_SCRIPTS_CLEAR_BOOT || page == RYZ_V5_SCRIPTS_PICKER) {
                view.catalog = catalog;
                /* A fresh explicit confirmation may target a fresh catalog;
                 * a previous accepted request is never reset mid-flight. */
                if (last_write_state != RYZ_V5_SCRIPTS_PENDING &&
                    last_write_state != RYZ_V5_SCRIPTS_UNKNOWN && !view.recovery_required) {
                    view.operation = RYZ_V5_SCRIPTS_OP_NONE;
                    view.result = RYZ_V5_SCRIPTS_IDLE; view.error = ESP_OK;
                    view.batch_total = view.batch_removed = 0; view.batch_complete = false;
                    view.cleanup_error = ESP_OK; view.failed_name[0] = 0;
                }
            }
        }
        view.page = page;
        read_reader();
        if (got_result && ready.request.operation == RYZ_V5_SCRIPTS_OP_NONE && ready.request.session == session &&
            ready.request.revision == catalog.revision && ready_catalog()) {
            view.boot_known = ready.error == ESP_OK;
            view.boot_bytes_known = view.boot_known;
            view.boot_name[0] = 0; view.boot_bytes = 0;
            view.boot_revision = 0; memset(view.boot_sha256, 0, sizeof(view.boot_sha256));
            if (view.boot_known && ready.boot_exists) {
                strcpy(view.boot_name, "boot.lua"); view.boot_bytes = ready.boot.entry.bytes;
                view.boot_revision = ready.request.revision;
                memcpy(view.boot_sha256, ready.boot.sha256, sizeof(view.boot_sha256));
            }
            if (ready.error == ESP_ERR_TIMEOUT && boot_attempts < 3) boot_retry_ticks = 50;
            else if (ready.error != ESP_OK) boot_attempts = 3;
        }
        permissions();
        update_generation();
        if (got_intent) consume_intent(&intent);
    }
    if (queue_write && (!open || !same_nav(queued_navigation, navigation) ||
        view.page != (last_write.request.operation == RYZ_V5_SCRIPTS_OP_CLEAR_BOOT ?
            RYZ_V5_SCRIPTS_CLEAR_BOOT : RYZ_V5_SCRIPTS_PICKER) || !ready_catalog() ||
        catalog.revision != last_write.request.revision)) {
        queue_write = false;
        last_write_state = RYZ_V5_SCRIPTS_FAILED;
        last_write.error = ESP_ERR_INVALID_STATE;
        copy_write_result();
    }
    if (got_result && ready.request.operation != RYZ_V5_SCRIPTS_OP_NONE &&
        ready.request.id == last_write.request.id) {
        last_write = ready;
        last_write_state = ready.mutation.commit == RYZ_SCRIPT_STORE_COMMIT_UNKNOWN ? RYZ_V5_SCRIPTS_UNKNOWN :
            ready.mutation.commit == RYZ_SCRIPT_STORE_COMMITTED ? RYZ_V5_SCRIPTS_COMMITTED : RYZ_V5_SCRIPTS_FAILED;
        copy_write_result();
        view.show_saved = open && ready.request.session == session && same_nav(queued_navigation, navigation) &&
            view.page == RYZ_V5_SCRIPTS_PICKER && ready.request.operation == RYZ_V5_SCRIPTS_OP_BOOT &&
            last_write_state == RYZ_V5_SCRIPTS_COMMITTED && ready.error == ESP_OK;
        if (open) {
            clear_selection(); clear_boot();
            request_page(0, 0);
            view.scroll_y = 0;
            view.error = last_write.error;
        }
    }
    permissions();
    update_generation();
    if (take()) {
        if (queue_write) {
            if (slot != SLOT_IDLE && !(slot == SLOT_REQUESTED && request.operation == RYZ_V5_SCRIPTS_OP_NONE)) {
                last_write_state = RYZ_V5_SCRIPTS_FAILED;
                last_write.error = ESP_ERR_TIMEOUT;
                copy_write_result();
            } else {
                if (slot == SLOT_REQUESTED && boot_attempts) --boot_attempts;
                last_write.request.generation = view.generation;
                request = last_write.request; slot = SLOT_REQUESTED;
            }
            queue_write = false;
            permissions(); update_generation();
        }
        published = view; published_open = open; published_session = session;
        atomic_store(&live_view, open && view.generation != 0);
        if (boot_attempt_revision != catalog.revision) {
            boot_attempt_revision = catalog.revision; boot_attempts = boot_retry_ticks = 0;
        }
        if (open && ready_catalog() && !view.boot_known && boot_attempts < 3 && !boot_retry_ticks &&
            slot == SLOT_IDLE) {
            request = (request_t){.session = session, .revision = catalog.revision};
            slot = SLOT_REQUESTED; ++boot_attempts;
        }
        give();
    }
}

static void read_boot(result_t *out)
{
    ryz_script_store_status_t before, after;
    out->error = ryz_script_store_status(&before);
    if (out->error != ESP_OK) return;
    if (!before.ready || before.recovery_required || before.revision != out->request.revision) {
        out->error = ESP_ERR_INVALID_STATE; return;
    }
    out->error = ryz_script_store_describe("boot.lua", &out->boot);
    out->boot_exists = out->error == ESP_OK;
    if (out->error == ESP_ERR_NOT_FOUND) out->error = ESP_OK;
    if (out->error != ESP_OK) return;
    out->error = ryz_script_store_status(&after);
    if (out->error == ESP_OK && (!after.ready || after.recovery_required ||
        before.revision != after.revision)) out->error = ESP_ERR_INVALID_STATE;
    if (out->error != ESP_OK) {
        memset(&out->boot, 0, sizeof(out->boot)); out->boot_exists = false;
    }
}

void ryz_workbench_scripts_rx_tick(const ryz_workbench_scripts_write_guard_t *guard)
{
    if (!atomic_load(&initialized)) return;
    if (!rx_has_result) {
        if (!take()) return;
        if (slot != SLOT_REQUESTED) { give(); return; }
        rx_result = (result_t){.request = request, .error = ESP_ERR_INVALID_STATE};
        const bool writing = request.operation != RYZ_V5_SCRIPTS_OP_NONE;
        const ryz_v5_scripts_page_t expected_page = request.operation == RYZ_V5_SCRIPTS_OP_CLEAR_BOOT ?
            RYZ_V5_SCRIPTS_CLEAR_BOOT : RYZ_V5_SCRIPTS_PICKER;
        const bool current = atomic_load(&live_view) && published_open && published_session == request.session &&
            (!writing || (published.page == expected_page &&
             published.generation == request.generation && published.catalog.revision == request.revision &&
             published.result == RYZ_V5_SCRIPTS_PENDING && !has_intent));
        slot = SLOT_WORKING;
        give();
        if (!writing) {
            if (current) read_boot(&rx_result);
        } else if (!current) rx_result.error = ESP_ERR_INVALID_STATE;
        else if (!guard || !guard->try_begin_write || !guard->end_write)
            rx_result.error = ESP_ERR_NOT_SUPPORTED;
        else if (!guard->try_begin_write(guard->context)) rx_result.error = ESP_ERR_TIMEOUT;
        else {
            /* The shared guard serializes every Job admission and Store writer.
             * Check the confirmed revision after acquiring that lease; the
             * Store then repeats target SHA CAS atomically under its own lock. */
            ryz_script_store_status_t status;
            rx_result.error = ryz_script_store_status(&status);
            if (rx_result.error == ESP_OK && (!status.ready || status.recovery_required ||
                status.revision != rx_result.request.revision)) rx_result.error = ESP_ERR_INVALID_STATE;
            if (rx_result.error == ESP_OK) {
                if (rx_result.request.operation == RYZ_V5_SCRIPTS_OP_CLEAR_BOOT)
                    rx_result.error = ryz_script_store_remove("boot.lua", rx_result.request.sha256, &rx_result.mutation);
                else
                    rx_result.error = ryz_script_store_copy_boot(rx_result.request.name, rx_result.request.sha256,
                        rx_result.request.revision, &rx_result.mutation);
            }
            guard->end_write(guard->context);
        }
        rx_has_result = true;
    }
    if (take()) {
        result = rx_result; slot = SLOT_READY;
        rx_has_result = false; give();
    }
}
