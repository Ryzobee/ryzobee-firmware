#include "ryz_monitor_source.h"
#include "ryz_monitor_stream.h"
#include "ryz_log.h"
#include <stdatomic.h>

/* Compatibility consumer only. Capture/privacy/sink ownership is provided
 * by the generic logger; Monitor still owns its session and pause/history. */
static atomic_bool s_enabled;
/* The shared source serializes begin/final system notifications. Gap and
 * application callbacks can interleave but never mutate this token. */
static ryz_monitor_capture_token_t s_token;
static bool s_captured;

static void diagnostic(const ryz_log_event_t *event)
{
    if (event->application) return;
    if (event->gap) {
        if (atomic_load_explicit(&s_enabled, memory_order_acquire)) ryz_monitor_stream_gap();
        return;
    }
    if (event->begin) {
        s_captured = atomic_load_explicit(&s_enabled, memory_order_acquire) &&
            ryz_monitor_stream_token(&s_token) == ESP_OK && s_token.source == RYZ_MONITOR_SYSTEM;
        return;
    }
    if (!s_captured) return;
    s_captured = false;
    if (event->filtered) {
        (void)ryz_monitor_stream_filtered(s_token);
        return;
    }
    size_t kept = event->length < RYZ_MONITOR_RECORD_BYTES ? event->length : RYZ_MONITOR_RECORD_BYTES;
    if (kept) (void)ryz_monitor_stream_append(s_token, event->bytes, kept,
        event->original_length, event->captured_ms);
}

esp_err_t ryz_monitor_log_install(void)
{
    return ryz_log_add_observer(diagnostic);
}

void ryz_monitor_log_capture(bool enabled)
{
    atomic_store_explicit(&s_enabled, enabled, memory_order_release);
}
