#include "ryz_apps.h"
#include "ryz_apps_platform.h"
#include "ryz_apps_reader.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

enum { STOPPED, STARTING, RUNNING };
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(STOPPED);

typedef struct {
    uint64_t id;
    ryz_apps_view_t view;
    size_t offset;
    size_t limit;
    uint32_t revision;
    bool automatic;
    char name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
} request_t;

static request_t s_desired;
/* Reader/UI task copy cache, never an ISR or DMA buffer. Its protecting
 * mutex and lifecycle state stay internal; accesses require the cache on. */
static EXT_RAM_BSS_ATTR ryz_apps_snapshot_t s_snapshot;
static uint64_t s_next_id;

static bool running(void)
{
    return atomic_load_explicit(&s_lifecycle, memory_order_acquire) == RUNNING;
}

/* Requires only the cache lock. No allocation, filesystem or UI calls. */
static bool schedule(request_t request, uint64_t *out_id)
{
    if (s_next_id == UINT64_MAX) return false;
    request.id = ++s_next_id;
    s_desired = request;
    s_snapshot = (ryz_apps_snapshot_t){
        .request_id = request.id, .view = request.view,
        .state = RYZ_APPS_LOADING,
    };
    if (out_id != NULL) *out_id = request.id;
    return true;
}

static esp_err_t read_page(const request_t *request,
                           ryz_script_store_page_t *out)
{
    ryz_script_store_status_t status;
    esp_err_t error = ryz_script_store_status(&status);
    if (error != ESP_OK) return error;
    if (!status.ready || status.recovery_required ||
        (request->revision && request->revision != status.revision)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t revision = status.revision;
    error = ryz_script_store_list(request->offset, request->limit, revision, out);
    const bool empty_tail = error == ESP_OK && request->offset && out->count == 0;
    if ((error == ESP_ERR_INVALID_ARG || empty_tail) &&
        request->automatic && request->offset) {
        /* The last page may disappear after a deletion. Never silently alter
         * an explicit user's paging request; only a cache refresh can reset. */
        error = ryz_script_store_list(0, request->limit, revision, out);
    } else if (empty_tail) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error != ESP_OK) return error;
    error = ryz_script_store_status(&status);
    if (error != ESP_OK) return error;
    return status.ready && !status.recovery_required &&
        status.revision == revision ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void read_request(const request_t *request, ryz_apps_snapshot_t *result)
{
    *result = (ryz_apps_snapshot_t){
        .request_id = request->id, .completed_id = request->id,
        .view = request->view, .state = RYZ_APPS_FAILED,
    };
    esp_err_t error = request->view == RYZ_APPS_VIEW_CATALOG ?
        read_page(request, &result->page) :
        ryz_apps_read_detail(request->name, request->revision, &result->detail);
    ryz_script_store_status_t status;
    esp_err_t status_error = ryz_script_store_status(&status);
    if (status_error == ESP_OK) {
        result->store_status_valid = true;
        result->store_ready = status.ready;
        result->recovery_required = status.recovery_required;
        const uint32_t revision = request->view == RYZ_APPS_VIEW_CATALOG ?
            result->page.revision : result->detail.revision;
        if (error == ESP_OK && (!status.ready || status.recovery_required ||
                               revision != status.revision)) {
            error = ESP_ERR_INVALID_STATE;
        }
    } else if (error == ESP_OK) {
        error = status_error;
    }
    result->error = error;
    if (error == ESP_OK) {
        result->state = request->view == RYZ_APPS_VIEW_CATALOG &&
            result->page.total == 0 ? RYZ_APPS_EMPTY : RYZ_APPS_READY;
    } else {
        result->state = error == ESP_ERR_INVALID_STATE && result->store_ready &&
            !result->recovery_required ? RYZ_APPS_STALE : RYZ_APPS_FAILED;
        memset(&result->page, 0, sizeof(result->page));
        memset(&result->detail, 0, sizeof(result->detail));
    }
}

static void observe_store(void)
{
    ryz_apps_platform_lock();
    const uint64_t id = s_snapshot.request_id;
    const bool valid = s_snapshot.state == RYZ_APPS_READY ||
                       s_snapshot.state == RYZ_APPS_EMPTY;
    ryz_apps_platform_unlock();
    if (!valid) return;

    /* Status is copy-only, but still take it OUTSIDE the cache lock. A busy
     * Store is a missed observation, not proof of unready/empty storage. */
    ryz_script_store_status_t status;
    const esp_err_t error = ryz_script_store_status(&status);
    if (error == ESP_ERR_TIMEOUT) return;

    ryz_apps_platform_lock();
    if (s_snapshot.request_id != id ||
        (s_snapshot.state != RYZ_APPS_READY && s_snapshot.state != RYZ_APPS_EMPTY)) {
        ryz_apps_platform_unlock();
        return;
    }
    if (error != ESP_OK || !status.ready || status.recovery_required) {
        s_snapshot.state = RYZ_APPS_FAILED;
        s_snapshot.error = error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
        s_snapshot.store_status_valid = error == ESP_OK;
        s_snapshot.store_ready = error == ESP_OK && status.ready;
        s_snapshot.recovery_required = error == ESP_OK && status.recovery_required;
        memset(&s_snapshot.page, 0, sizeof(s_snapshot.page));
        memset(&s_snapshot.detail, 0, sizeof(s_snapshot.detail));
    } else {
        const uint32_t revision = s_snapshot.view == RYZ_APPS_VIEW_CATALOG ?
            s_snapshot.page.revision : s_snapshot.detail.revision;
        if (status.revision != revision) {
            if (s_snapshot.view == RYZ_APPS_VIEW_CATALOG) {
                request_t refresh = s_desired;
                refresh.offset = s_snapshot.page.offset;
                refresh.revision = 0;
                refresh.automatic = true;
                if (!schedule(refresh, NULL)) {
                    s_snapshot.state = RYZ_APPS_FAILED;
                    s_snapshot.error = ESP_ERR_INVALID_STATE;
                    memset(&s_snapshot.page, 0, sizeof(s_snapshot.page));
                }
            } else {
                /* Deliberately do NOT read/re-arm a new version of the same
                 * selected file. The caller must refresh and select again. */
                s_snapshot.state = RYZ_APPS_STALE;
                s_snapshot.error = ESP_ERR_INVALID_STATE;
                memset(&s_snapshot.detail, 0, sizeof(s_snapshot.detail));
            }
        }
    }
    ryz_apps_platform_unlock();
}

static void reader_task(void *unused)
{
    (void)unused;
    for (;;) {
        observe_store();
        request_t request;
        ryz_apps_platform_lock();
        request = s_desired;
        const bool pending = s_snapshot.state == RYZ_APPS_LOADING;
        ryz_apps_platform_unlock();
        if (pending) {
            ryz_apps_snapshot_t result;
            read_request(&request, &result);
            ryz_apps_platform_lock();
            if (s_desired.id == request.id && s_desired.view == request.view &&
                s_snapshot.state == RYZ_APPS_LOADING) {
                s_snapshot = result;
            }
            ryz_apps_platform_unlock();
        }
        ryz_apps_platform_wait();
    }
}

esp_err_t ryz_apps_start(void)
{
    int expected = STOPPED;
    if (!atomic_compare_exchange_strong(&s_lifecycle, &expected, STARTING)) {
        return expected == RUNNING ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = ryz_apps_platform_init();
    if (error == ESP_OK) {
        s_desired = (request_t){0};
        s_snapshot = (ryz_apps_snapshot_t){0};
        s_next_id = 0;
        error = ryz_apps_platform_start(reader_task);
        if (error != ESP_OK) ryz_apps_platform_deinit();
    }
    atomic_store_explicit(&s_lifecycle,
                          error == ESP_OK ? RUNNING : STOPPED, memory_order_release);
    return error;
}

static esp_err_t submit(request_t request, uint64_t *out_id)
{
    if (!running()) return ESP_ERR_INVALID_STATE;
    ryz_apps_platform_lock();
    const bool accepted = schedule(request, out_id);
    ryz_apps_platform_unlock();
    if (!accepted) return ESP_ERR_INVALID_STATE;
    ryz_apps_platform_wake();
    return ESP_OK;
}

esp_err_t ryz_apps_request_page(size_t offset, size_t limit,
                               uint32_t expected_revision, uint64_t *out_id)
{
    if (out_id == NULL) return ESP_ERR_INVALID_ARG;
    *out_id = 0;
    if (offset > RYZ_SCRIPT_STORE_INDEX_MAX || limit == 0 ||
        limit > RYZ_SCRIPT_STORE_PAGE_MAX) return ESP_ERR_INVALID_ARG;
    const request_t request = {
        .view = RYZ_APPS_VIEW_CATALOG, .offset = offset, .limit = limit,
        .revision = expected_revision,
    };
    return submit(request, out_id);
}

esp_err_t ryz_apps_request_detail(const char *name, uint32_t expected_revision,
                                 uint64_t *out_id)
{
    if (out_id == NULL) return ESP_ERR_INVALID_ARG;
    *out_id = 0;
    if (expected_revision == 0 || !ryz_script_store_valid_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    request_t request = {.view = RYZ_APPS_VIEW_DETAIL, .revision = expected_revision};
    strcpy(request.name, name);
    return submit(request, out_id);
}

esp_err_t ryz_apps_close(void)
{
    if (!running()) return ESP_ERR_INVALID_STATE;
    ryz_apps_platform_lock();
    s_desired = (request_t){0};
    s_snapshot = (ryz_apps_snapshot_t){0};
    ryz_apps_platform_unlock();
    ryz_apps_platform_wake();
    return ESP_OK;
}

esp_err_t ryz_apps_get_snapshot(ryz_apps_snapshot_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = (ryz_apps_snapshot_t){0};
    if (!running()) return ESP_ERR_INVALID_STATE;
    ryz_apps_platform_lock();
    *out = s_snapshot;
    ryz_apps_platform_unlock();
    return ESP_OK;
}
