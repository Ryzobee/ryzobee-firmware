#include "ryz_monitor_stream.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

typedef struct {
    ryz_monitor_record_t records[RYZ_MONITOR_CAPACITY];
    uint16_t head, count;
} window_t;
/* Task/cache-on data only: UART appends after its task-level driver read;
 * the audited Log V1 tap does not receive EARLY/DRAM (ISR/cache-off) logs.
 * Keep the atomic guards and small capture state in internal RAM below. */
static EXT_RAM_BSS_ATTR window_t s_live, s_frozen;
static ryz_monitor_stream_info_t s_info;
static uint32_t s_epoch, s_last_sequence;
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static atomic_bool s_accepting, s_gap;

static bool acquire(void)
{
    return !atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire);
}
static void release(void) { atomic_flag_clear_explicit(&s_lock, memory_order_release); }
static void add(uint64_t *value, uint64_t amount)
{
    *value = amount > UINT64_MAX - *value ? UINT64_MAX : *value + amount;
}
void ryz_monitor_stream_gap(void) { atomic_store_explicit(&s_gap, true, memory_order_relaxed); }
static bool valid_source(ryz_monitor_source_t source)
{
    return source == RYZ_MONITOR_SYSTEM || source == RYZ_MONITOR_UART;
}
static void info_locked(ryz_monitor_stream_info_t *out)
{
    *out = s_info;
    const window_t *view = s_info.paused ? &s_frozen : &s_live;
    out->retained_records = view->count;
    out->first_sequence = view->count ? view->records[view->head].sequence : 0;
    out->last_sequence = view->count ?
        view->records[(view->head + view->count - 1) % RYZ_MONITOR_CAPACITY].sequence : 0;
    out->capture_gap_since_boot = atomic_load_explicit(&s_gap, memory_order_relaxed);
}
static bool matches(ryz_monitor_capture_token_t token)
{
    return token.session_id && s_info.accepting && token.session_id == s_info.session_id &&
           token.epoch == s_epoch && token.source == s_info.source;
}
static void erase_windows(void)
{
    memset(&s_live, 0, sizeof(s_live));
    memset(&s_frozen, 0, sizeof(s_frozen));
}

esp_err_t ryz_monitor_stream_reset(uint32_t session, uint32_t revision,
                                  ryz_monitor_source_t source)
{
    if (!session || !revision || !valid_source(source)) return ESP_ERR_INVALID_ARG;
    if (!acquire()) return ESP_ERR_NOT_FINISHED;
    if (s_info.accepting || session <= s_info.session_id) {
        release(); return ESP_ERR_INVALID_STATE;
    }
    erase_windows();
    memset(&s_info, 0, sizeof(s_info));
    s_info.session_id = session;
    s_info.config_revision = revision;
    s_info.source = source;
    s_info.view_generation = 1;
    s_epoch = 1;
    s_last_sequence = 0;
    atomic_store_explicit(&s_accepting, false, memory_order_release);
    release();
    return ESP_OK;
}
esp_err_t ryz_monitor_stream_reconfigure(uint32_t session, uint32_t revision,
                                        ryz_monitor_source_t source)
{
    if (!session || !revision || !valid_source(source)) return ESP_ERR_INVALID_ARG;
    if (!acquire()) return ESP_ERR_NOT_FINISHED;
    if (session != s_info.session_id || s_info.accepting || s_info.sequence_exhausted ||
        s_info.view_generation == UINT32_MAX || s_epoch == UINT32_MAX ||
        revision <= s_info.config_revision) {
        release(); return ESP_ERR_INVALID_STATE;
    }
    erase_windows();
    s_info.config_revision = revision;
    s_info.source = source;
    ++s_info.view_generation;
    ++s_epoch;
    s_info.accepting = true;
    atomic_store_explicit(&s_accepting, true, memory_order_release);
    release();
    return ESP_OK;
}
esp_err_t ryz_monitor_stream_accept(uint32_t session, bool enabled)
{
    if (!session) return ESP_ERR_INVALID_ARG;
    if (!acquire()) return ESP_ERR_NOT_FINISHED;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (session == s_info.session_id) {
        if (s_info.accepting == enabled) error = ESP_OK;
        else if (!enabled || (s_epoch != UINT32_MAX && !s_info.sequence_exhausted)) {
            /* Disabling is always possible, even at epoch exhaustion. */
            if (s_epoch != UINT32_MAX) ++s_epoch;
            s_info.accepting = enabled;
            atomic_store_explicit(&s_accepting, enabled, memory_order_release);
            error = ESP_OK;
        }
    }
    release();
    return error;
}
esp_err_t ryz_monitor_stream_token(ryz_monitor_capture_token_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!atomic_load_explicit(&s_accepting, memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!acquire()) { ryz_monitor_stream_gap(); return ESP_ERR_NOT_FINISHED; }
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_info.accepting) {
        *out = (ryz_monitor_capture_token_t){s_info.session_id, s_epoch, s_info.source};
        error = ESP_OK;
    }
    release();
    return error;
}
esp_err_t ryz_monitor_stream_append(ryz_monitor_capture_token_t token,
                                  const void *bytes, size_t kept, size_t original,
                                  int64_t captured_ms)
{
    if (!bytes || !kept || kept > RYZ_MONITOR_RECORD_BYTES || original < kept ||
        captured_ms < 0) return ESP_ERR_INVALID_ARG;
    if (!acquire()) { ryz_monitor_stream_gap(); return ESP_ERR_NOT_FINISHED; }
    if (!matches(token)) { release(); return ESP_ERR_INVALID_STATE; }
    if (s_last_sequence == UINT32_MAX) {
        s_info.sequence_exhausted = true;
        s_info.accepting = false;
        atomic_store_explicit(&s_accepting, false, memory_order_release);
        release(); return ESP_ERR_INVALID_STATE;
    }
    if (s_live.count == RYZ_MONITOR_CAPACITY) {
        add(&s_info.overwritten_chunks, 1);
        add(&s_info.overwritten_bytes, s_live.records[s_live.head].length);
        s_live.head = (s_live.head + 1) % RYZ_MONITOR_CAPACITY;
        --s_live.count;
    }
    ryz_monitor_record_t *record = &s_live.records[(s_live.head + s_live.count) % RYZ_MONITOR_CAPACITY];
    memset(record, 0, sizeof(*record));
    record->sequence = ++s_last_sequence;
    record->captured_ms = captured_ms;
    record->length = (uint16_t)kept;
    record->truncated = original > kept;
    memcpy(record->bytes, bytes, kept);
    ++s_live.count;
    add(&s_info.chunks, 1);
    add(&s_info.bytes, original);
    add(&s_info.truncated_bytes, original - kept);
    release();
    return ESP_OK;
}
esp_err_t ryz_monitor_stream_filtered(ryz_monitor_capture_token_t token)
{
    if (!acquire()) { ryz_monitor_stream_gap(); return ESP_ERR_NOT_FINISHED; }
    if (!matches(token)) { release(); return ESP_ERR_INVALID_STATE; }
    add(&s_info.filtered_logs, 1);
    release();
    return ESP_OK;
}
esp_err_t ryz_monitor_stream_info(ryz_monitor_stream_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!acquire()) return ESP_ERR_NOT_FINISHED;
    if (!s_info.session_id) { release(); return ESP_ERR_INVALID_STATE; }
    info_locked(out);
    release();
    return ESP_OK;
}
esp_err_t ryz_monitor_stream_page(uint32_t session, uint32_t generation,
                                uint32_t after_sequence, ryz_monitor_page_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session || !generation) return ESP_ERR_INVALID_ARG;
    if (!acquire()) return ESP_ERR_NOT_FINISHED;
    if (session != s_info.session_id || generation != s_info.view_generation) {
        release(); return ESP_ERR_INVALID_STATE;
    }
    const window_t *view = s_info.paused ? &s_frozen : &s_live;
    uint32_t last = view->count ?
        view->records[(view->head + view->count - 1) % RYZ_MONITOR_CAPACITY].sequence : 0;
    if (after_sequence > last) { release(); return ESP_ERR_INVALID_ARG; }
    info_locked(&out->stream);
    out->next_sequence = after_sequence;
    out->gap = after_sequence && out->stream.first_sequence > after_sequence &&
               out->stream.first_sequence - after_sequence > 1;
    for (unsigned i = 0; i < view->count; ++i) {
        const ryz_monitor_record_t *record = &view->records[(view->head + i) % RYZ_MONITOR_CAPACITY];
        if (record->sequence <= after_sequence) continue;
        if (out->count == RYZ_MONITOR_PAGE_RECORDS) { out->more = true; break; }
        out->records[out->count++] = *record;
        out->next_sequence = record->sequence;
    }
    release();
    return ESP_OK;
}
esp_err_t ryz_monitor_stream_pause(uint32_t session, uint32_t generation,
                                 bool paused, uint32_t *out_generation)
{
    if (!out_generation) return ESP_ERR_INVALID_ARG;
    *out_generation = 0;
    if (!session || !generation) return ESP_ERR_INVALID_ARG;
    if (!acquire()) return ESP_ERR_NOT_FINISHED;
    if (session != s_info.session_id || generation != s_info.view_generation ||
        (paused != s_info.paused && generation == UINT32_MAX)) {
        release(); return ESP_ERR_INVALID_STATE;
    }
    if (paused != s_info.paused) {
        if (paused) s_frozen = s_live;
        else memset(&s_frozen, 0, sizeof(s_frozen));
        s_info.paused = paused;
        ++s_info.view_generation;
    }
    *out_generation = s_info.view_generation;
    release();
    return ESP_OK;
}
esp_err_t ryz_monitor_stream_clear(uint32_t session, uint32_t generation,
                                 uint32_t *out_generation)
{
    if (!out_generation) return ESP_ERR_INVALID_ARG;
    *out_generation = 0;
    if (!session || !generation) return ESP_ERR_INVALID_ARG;
    if (!acquire()) return ESP_ERR_NOT_FINISHED;
    if (session != s_info.session_id || generation != s_info.view_generation ||
        generation == UINT32_MAX || s_epoch == UINT32_MAX) {
        release(); return ESP_ERR_INVALID_STATE;
    }
    erase_windows();
    ++s_epoch;
    *out_generation = ++s_info.view_generation;
    release();
    return ESP_OK;
}
