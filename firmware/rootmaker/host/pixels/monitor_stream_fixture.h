#pragma once
/* Synthetic byte-stream environment for the real Lua script. This fixture
 * knows UART/log handles and bytes, never pause/history/config UI semantics. */
#include <assert.h>
#include <string.h>
#include "ryz_peripheral.h"

#define MONITOR_FIXTURE_BYTES 4096U
typedef struct {
    uint8_t bytes[MONITOR_FIXTURE_BYTES];
    size_t head, count;
    uint32_t identity, handle, dropped, error_events;
    ryz_peripheral_kind_t kind;
    ryz_peripheral_config_t config;
    unsigned opened, closed, reads;
    unsigned open_failures, close_failures, read_failures;
    bool unavailable, loss_possible, closing;
} monitor_stream_fixture_t;

static inline void monitor_fixture_init(monitor_stream_fixture_t *f)
{ memset(f, 0, sizeof(*f)); f->identity = UINT32_C(4000000000); }

static inline void monitor_fixture_feed(monitor_stream_fixture_t *f, const char *data, size_t count)
{
    if (!f->handle) return;
    for (size_t i = 0; i < count; ++i) {
        if (f->count == MONITOR_FIXTURE_BYTES) {
            f->head = (f->head + 1U) % MONITOR_FIXTURE_BYTES; --f->count;
            if (f->dropped != UINT32_MAX) ++f->dropped;
            f->loss_possible = true;
        }
        f->bytes[(f->head + f->count++) % MONITOR_FIXTURE_BYTES] = (uint8_t)data[i];
    }
}

static inline ryz_peripheral_result_t monitor_fixture_call(void *context,
    const ryz_peripheral_request_t *q, ryz_peripheral_reply_t *r)
{
    monitor_stream_fixture_t *f = context;
    memset(r, 0, sizeof(*r));
    if (f->unavailable) return RYZ_PERIPHERAL_UNAVAILABLE;
    if (q->kind != RYZ_PERIPHERAL_LOG && q->kind != RYZ_PERIPHERAL_UART)
        return RYZ_PERIPHERAL_UNAVAILABLE;
    if (q->op == RYZ_PERIPHERAL_OPEN) {
        if (f->open_failures) { --f->open_failures; return RYZ_PERIPHERAL_FAILED; }
        if (f->handle) return RYZ_PERIPHERAL_BUSY;
        if (q->kind == RYZ_PERIPHERAL_UART) {
            /* A Monitor user never authorizes TX simply by selecting a port. */
            assert(q->config.uart.tx == -1 && q->config.uart.rx >= 0);
            if (q->config.uart.rx == 19 || q->config.uart.rx == 20)
                return RYZ_PERIPHERAL_INVALID;
        }
        f->kind = q->kind; f->config = q->config;
        f->handle = ++f->identity; r->handle = f->handle; ++f->opened;
        f->head = f->count = 0; f->dropped = f->error_events = 0; f->loss_possible = false;
        f->closing = false;
        return RYZ_PERIPHERAL_OK;
    }
    if (!f->handle || q->handle != f->handle || q->kind != f->kind)
        return RYZ_PERIPHERAL_CLOSED;
    if (q->op == RYZ_PERIPHERAL_CLOSE) {
        f->closing = true;
        if (f->close_failures) { --f->close_failures; return RYZ_PERIPHERAL_FAILED; }
        f->handle = 0; f->head = f->count = 0; ++f->closed;
        return RYZ_PERIPHERAL_OK;
    }
    if (f->closing) return RYZ_PERIPHERAL_CLOSED;
    if (q->op != RYZ_PERIPHERAL_READ) return RYZ_PERIPHERAL_UNSUPPORTED;
    assert(q->read_length <= RYZ_PERIPHERAL_BUFFER_MAX && q->timeout_ms == 0);
    ++f->reads;
    if (f->read_failures) { --f->read_failures; return RYZ_PERIPHERAL_TIMEOUT; }
    r->length = f->count < q->read_length ? f->count : q->read_length;
    for (size_t i = 0; i < r->length; ++i) {
        r->data[i] = f->bytes[f->head];
        f->head = (f->head + 1U) % MONITOR_FIXTURE_BYTES;
    }
    f->count -= r->length;
    r->dropped = f->dropped; r->error_events = f->error_events;
    r->loss_possible = f->loss_possible;
    return RYZ_PERIPHERAL_OK;
}
