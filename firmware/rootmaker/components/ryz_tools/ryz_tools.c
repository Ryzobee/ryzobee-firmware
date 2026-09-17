#include "ryz_tools.h"
#include <stdatomic.h>
#include <string.h>

typedef struct {
    uint32_t owner, native_id, rgb_lease;
    bool closing, requested, failed;
    esp_err_t error;
} slot_t;
static slot_t s_slots[RYZ_TOOL_COUNT];
static uint32_t s_last_session, s_current;
/* Admission gate, NOT an RTOS critical section/spin loop: contenders return
 * immediately and never disable scheduling/interrupts. It spans only core
 * cache copies or command admission (possibly lazy OS allocation), never
 * driver work. Raw workers do not call back into this Module. */
static atomic_flag s_gate = ATOMIC_FLAG_INIT;
static bool enter(void)
{ return !atomic_flag_test_and_set_explicit(&s_gate, memory_order_acquire); }
static void leave(void) { atomic_flag_clear_explicit(&s_gate, memory_order_release); }
static bool i2c_active(ryz_i2c_scan_phase_t phase)
{
    return phase == RYZ_I2C_SCAN_QUEUED || phase == RYZ_I2C_SCAN_RUNNING ||
        phase == RYZ_I2C_SCAN_CANCELLING || phase == RYZ_I2C_SCAN_RELEASING;
}
static bool rgb_active(ryz_rgb_phase_t phase)
{
    return phase == RYZ_RGB_QUEUED || phase == RYZ_RGB_SENDING || phase == RYZ_RGB_CLEANING;
}
static bool monitor_active(const ryz_monitor_snapshot_t *state)
{
    return state->operation_pending || state->source_active || state->resources_held ||
        state->phase == RYZ_MONITOR_STARTING || state->phase == RYZ_MONITOR_RUNNING ||
        state->phase == RYZ_MONITOR_RECONFIGURING || state->phase == RYZ_MONITOR_STOPPING;
}
static esp_err_t cached(ryz_tool_t tool, ryz_tool_reply_t *out)
{
    esp_err_t error;
    if (tool == RYZ_TOOL_I2C) {
        error = ryz_i2c_scan_get_snapshot(&out->state.i2c);
        if (error == ESP_ERR_INVALID_STATE) {
            memset(&out->state.i2c, 0, sizeof(out->state.i2c));
            out->state.i2c.config = RYZ_I2C_SCAN_DEFAULT_CONFIG;
            out->state.i2c.config_revision = 1;
        }
    } else if (tool == RYZ_TOOL_RGB) {
        error = ryz_rgb_get_snapshot(&out->state.rgb);
    } else {
        error = ryz_monitor_get_snapshot(&out->state.monitor);
        if (error == ESP_ERR_INVALID_STATE) {
            memset(&out->state.monitor, 0, sizeof(out->state.monitor));
            out->state.monitor.config = RYZ_MONITOR_DEFAULT_CONFIG;
            out->state.monitor.config_revision = 1;
        }
    }
    out->started = error == ESP_OK;
    return error == ESP_ERR_INVALID_STATE ? ESP_OK : error;
}
static bool inactive(ryz_tool_t tool, const ryz_tool_reply_t *out)
{
    if (!out->started) return true;
    if (tool == RYZ_TOOL_I2C) {
        ryz_i2c_scan_phase_t phase = out->state.i2c.phase;
        return (phase == RYZ_I2C_SCAN_IDLE || phase == RYZ_I2C_SCAN_COMPLETED ||
                phase == RYZ_I2C_SCAN_CANCELLED || phase == RYZ_I2C_SCAN_FAILED) &&
            !out->state.i2c.resources_held;
    }
    if (tool == RYZ_TOOL_RGB) {
        ryz_rgb_phase_t phase = out->state.rgb.phase;
        return (phase == RYZ_RGB_IDLE || phase == RYZ_RGB_COMPLETED ||
                phase == RYZ_RGB_FAILED || phase == RYZ_RGB_CLEANED) &&
            !out->state.rgb.resources_held;
    }
    ryz_monitor_phase_t phase = out->state.monitor.phase;
    return (phase == RYZ_MONITOR_IDLE || phase == RYZ_MONITOR_STOPPED || phase == RYZ_MONITOR_FAILED) &&
        !monitor_active(&out->state.monitor);
}
static uint32_t native_id(ryz_tool_t tool, const ryz_tool_reply_t *out)
{
    return tool == RYZ_TOOL_I2C ? out->state.i2c.scan_id :
        tool == RYZ_TOOL_RGB ? out->state.rgb.request_id : out->state.monitor.session_id;
}
static bool parked_rgb(const ryz_tool_reply_t *out)
{
    return out->started && out->state.rgb.phase == RYZ_RGB_COMPLETED &&
        out->state.rgb.output_known && out->state.rgb.error == ESP_OK &&
        out->state.rgb.cleanup_error == ESP_OK;
}
static esp_err_t validate(const ryz_tool_request_t *request, ryz_tool_t *tool)
{
    switch (request->action) {
    case RYZ_TOOL_I2C_CONFIGURE:
        *tool = RYZ_TOOL_I2C;
        return request->expected_revision ? ryz_i2c_scan_validate_config(&request->config.i2c) : ESP_ERR_INVALID_ARG;
    case RYZ_TOOL_I2C_START:
        *tool = RYZ_TOOL_I2C;
        return request->expected_revision ? ESP_OK : ESP_ERR_INVALID_ARG;
    case RYZ_TOOL_I2C_STATUS: case RYZ_TOOL_I2C_CANCEL:
        *tool = RYZ_TOOL_I2C;
        return request->action == RYZ_TOOL_I2C_CANCEL && !request->operation_id ? ESP_ERR_INVALID_ARG : ESP_OK;
    case RYZ_TOOL_RGB_SET: case RYZ_TOOL_RGB_STATUS:
        *tool = RYZ_TOOL_RGB;
        return ESP_OK;
    case RYZ_TOOL_MONITOR_CONFIGURE:
        *tool = RYZ_TOOL_MONITOR;
        return request->expected_revision ? ryz_monitor_validate_config(&request->config.monitor) : ESP_ERR_INVALID_ARG;
    case RYZ_TOOL_MONITOR_START:
        *tool = RYZ_TOOL_MONITOR;
        return request->expected_revision ? ESP_OK : ESP_ERR_INVALID_ARG;
    case RYZ_TOOL_MONITOR_STATUS:
        *tool = RYZ_TOOL_MONITOR;
        return ESP_OK;
    case RYZ_TOOL_MONITOR_STOP: case RYZ_TOOL_MONITOR_PAUSE:
    case RYZ_TOOL_MONITOR_CLEAR: case RYZ_TOOL_MONITOR_READ:
        *tool = RYZ_TOOL_MONITOR;
        return request->operation_id && (request->action == RYZ_TOOL_MONITOR_STOP || request->view_generation) ?
            ESP_OK : ESP_ERR_INVALID_ARG;
    default: return ESP_ERR_INVALID_ARG;
    }
}
esp_err_t ryz_tools_open(uint32_t *out_session)
{
    if (!out_session) return ESP_ERR_INVALID_ARG;
    *out_session = 0;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!s_current && s_last_session != UINT32_MAX) {
        *out_session = s_current = ++s_last_session;
        error = ESP_OK;
    }
    leave();
    return error;
}
static esp_err_t perform(slot_t *slot, const ryz_tool_request_t *request, ryz_tool_reply_t *out)
{
    esp_err_t error;
    switch (request->action) {
    case RYZ_TOOL_I2C_CONFIGURE:
        return ryz_i2c_scan_configure(&request->config.i2c, request->expected_revision, &out->revision);
    case RYZ_TOOL_I2C_START: {
        ryz_i2c_scan_snapshot_t state;
        error = ryz_i2c_scan_get_snapshot(&state);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
        uint32_t revision = error == ESP_ERR_INVALID_STATE ? 1 : state.config_revision;
        if (revision != request->expected_revision) return ESP_ERR_INVALID_STATE;
        error = ryz_i2c_scan_start(&out->operation_id);
        if (error == ESP_OK) slot->native_id = out->operation_id;
        return error;
    }
    case RYZ_TOOL_I2C_STATUS: return cached(RYZ_TOOL_I2C, out);
    case RYZ_TOOL_I2C_CANCEL:
        return slot->native_id == request->operation_id ? ryz_i2c_scan_cancel(slot->native_id) : ESP_ERR_INVALID_STATE;
    case RYZ_TOOL_RGB_SET:
        error = ryz_rgb_lease_submit(slot->rgb_lease, request->config.rgb, &out->operation_id);
        if (error == ESP_OK) slot->native_id = out->operation_id;
        return error;
    case RYZ_TOOL_RGB_STATUS: return cached(RYZ_TOOL_RGB, out);
    case RYZ_TOOL_MONITOR_CONFIGURE: {
        ryz_monitor_snapshot_t state;
        error = ryz_monitor_get_snapshot(&state);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
        uint32_t session = error == ESP_OK ? state.session_id : 0;
        return ryz_monitor_configure(&request->config.monitor, request->expected_revision,
                                     session, &out->operation_id);
    }
    case RYZ_TOOL_MONITOR_START:
        error = ryz_monitor_start(request->expected_revision, &out->operation_id);
        if (error == ESP_OK) slot->native_id = out->operation_id;
        return error;
    case RYZ_TOOL_MONITOR_STATUS: return cached(RYZ_TOOL_MONITOR, out);
    default: break;
    }
    if (slot->native_id != request->operation_id) return ESP_ERR_INVALID_STATE;
    switch (request->action) {
    case RYZ_TOOL_MONITOR_STOP: return ryz_monitor_stop(slot->native_id, &out->operation_id);
    case RYZ_TOOL_MONITOR_PAUSE:
        return ryz_monitor_set_paused(slot->native_id, request->view_generation, request->paused, &out->view_generation);
    case RYZ_TOOL_MONITOR_CLEAR:
        return ryz_monitor_clear(slot->native_id, request->view_generation, &out->view_generation);
    case RYZ_TOOL_MONITOR_READ:
        return ryz_monitor_read(slot->native_id, request->view_generation, request->after_sequence, &out->state.page);
    default: return ESP_ERR_INVALID_ARG;
    }
}
esp_err_t ryz_tools_call(uint32_t session, const ryz_tool_request_t *request, ryz_tool_reply_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    /* Request and output are distinct caller-owned objects. */
    ryz_tool_request_t value = request ? *request : (ryz_tool_request_t){0};
    memset(out, 0, sizeof(*out));
    if (!session || !request) return ESP_ERR_INVALID_ARG;
    ryz_tool_t tool;
    esp_err_t error = validate(&value, &tool);
    if (error != ESP_OK) return error;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    slot_t *slot = &s_slots[tool];
    error = ESP_ERR_INVALID_STATE;
    if (session != s_current) goto done;
    if (slot->owner && slot->owner != session) { error = ESP_ERR_NOT_FINISHED; goto done; }
    if (!slot->owner) {
        error = cached(tool, out);
        if (error != ESP_OK) goto done;
        if (!inactive(tool, out) && !(tool == RYZ_TOOL_RGB && parked_rgb(out))) {
            error = ESP_ERR_NOT_FINISHED; goto done;
        }
        *slot = (slot_t){.owner = session};
        if (tool == RYZ_TOOL_RGB) {
            error = ryz_rgb_lease_open(&slot->rgb_lease);
            if (error != ESP_OK) { memset(slot, 0, sizeof(*slot)); goto done; }
        }
        memset(out, 0, sizeof(*out));
    }
    error = perform(slot, &value, out);
done:
    if (error != ESP_OK) memset(out, 0, sizeof(*out));
    leave();
    return error;
}
static void fail(slot_t *slot, esp_err_t error)
{ slot->failed = true; slot->error = error; }
static void progress(ryz_tool_t tool, slot_t *slot)
{
    if (slot->failed) return;
    if (tool == RYZ_TOOL_RGB) {
        esp_err_t error = ryz_rgb_lease_close(slot->rgb_lease);
        if (error == ESP_OK) memset(slot, 0, sizeof(*slot));
        else if (error == ESP_ERR_NOT_FINISHED || error == ESP_ERR_TIMEOUT) slot->requested = true;
        else fail(slot, error);
        return;
    }
    ryz_tool_reply_t reply = {0};
    esp_err_t error = cached(tool, &reply);
    if (error == ESP_ERR_NOT_FINISHED || error == ESP_ERR_TIMEOUT) return;
    if (error != ESP_OK) { fail(slot, error); return; }
    /* A never-started software-only configuration has nothing to cancel.
     * A formerly accepted native identity disappearing is not proof of release. */
    if (!reply.started && slot->native_id) { fail(slot, ESP_ERR_INVALID_STATE); return; }
    uint32_t id = native_id(tool, &reply);
    if (slot->native_id && slot->native_id != id) { fail(slot, ESP_ERR_INVALID_STATE); return; }
    if (inactive(tool, &reply)) { memset(slot, 0, sizeof(*slot)); return; }
    if (!slot->native_id) {
        /* A status/config-only job has not submitted a color and must not
         * destroy a previous controller's healthy parked allocation. */
        if (tool == RYZ_TOOL_RGB && parked_rgb(&reply)) { memset(slot, 0, sizeof(*slot)); return; }
        /* Only Monitor inactive-configure is asynchronous without a receive
         * session owned here. Let it finish; never stop an older session ID. */
        if (tool != RYZ_TOOL_MONITOR || reply.state.monitor.source_active || reply.state.monitor.resources_held ||
            !reply.state.monitor.operation_pending || reply.state.monitor.operation != RYZ_MONITOR_OP_CONFIGURE)
            fail(slot, ESP_ERR_INVALID_STATE);
        return;
    }
    bool active = tool == RYZ_TOOL_I2C ? i2c_active(reply.state.i2c.phase) :
        tool == RYZ_TOOL_RGB ? rgb_active(reply.state.rgb.phase) : reply.state.monitor.operation_pending;
    if (slot->requested) {
        if (!active) {
            esp_err_t cleanup = tool == RYZ_TOOL_I2C ? reply.state.i2c.cleanup_error :
                tool == RYZ_TOOL_RGB ? reply.state.rgb.cleanup_error : reply.state.monitor.cleanup_error;
            fail(slot, cleanup == ESP_OK ? ESP_ERR_INVALID_STATE : cleanup);
        }
        return;
    }
    if (tool == RYZ_TOOL_I2C) error = ryz_i2c_scan_cancel(slot->native_id);
    else if (tool == RYZ_TOOL_RGB) error = ryz_rgb_cleanup(slot->native_id);
    else { uint32_t operation; error = ryz_monitor_stop(slot->native_id, &operation); }
    if (error == ESP_OK) slot->requested = true;
    else if (error == ESP_ERR_INVALID_STATE) {
        /* A worker may finish naturally between cache read and cancel. A
         * rejected cancellation is not itself a cleanup failure: confirm the
         * SAME identity reached a known inactive, resource-free state. Never
         * retry a command here or use a newer controller's terminal state. */
        memset(&reply, 0, sizeof(reply));
        esp_err_t observed = cached(tool, &reply);
        if (observed == ESP_ERR_NOT_FINISHED || observed == ESP_ERR_TIMEOUT) return;
        if (observed == ESP_OK && reply.started && native_id(tool, &reply) == slot->native_id &&
            inactive(tool, &reply)) memset(slot, 0, sizeof(*slot));
        else fail(slot, observed == ESP_OK ? error : observed);
    }
    else if (error != ESP_ERR_NOT_FINISHED && error != ESP_ERR_TIMEOUT) fail(slot, error);
}
esp_err_t ryz_tools_close(uint32_t session, ryz_tools_close_result_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!session) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    if (session > s_last_session) { leave(); return ESP_ERR_INVALID_STATE; }
    if (s_current == session) s_current = 0;
    out->session_id = session;
    for (unsigned tool = 0; tool < RYZ_TOOL_COUNT; ++tool) {
        slot_t *slot = &s_slots[tool];
        if (slot->owner != session) continue;
        slot->closing = true;
        progress((ryz_tool_t)tool, slot);
        if (!slot->owner) continue;
        const uint8_t bit = (uint8_t)(1U << tool);
        out->remaining_mask |= bit;
        if (slot->failed) out->failed_mask |= bit;
        else out->pending_mask |= bit;
        out->errors[tool] = slot->error;
    }
    leave();
    return out->failed_mask ? ESP_FAIL : out->pending_mask ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
esp_err_t ryz_tools_retry_close(uint32_t session)
{
    if (!session) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    if (session > s_last_session || session == s_current) { leave(); return ESP_ERR_INVALID_STATE; }
    for (unsigned tool = 0; tool < RYZ_TOOL_COUNT; ++tool) {
        slot_t *slot = &s_slots[tool];
        if (slot->owner == session && slot->closing && slot->failed) {
            if (tool == RYZ_TOOL_RGB) {
                esp_err_t error = ryz_rgb_lease_retry_close(slot->rgb_lease);
                if (error != ESP_OK) { leave(); return error; }
            }
            slot->failed = slot->requested = false;
            slot->error = ESP_OK;
        }
    }
    leave();
    return ESP_OK;
}
esp_err_t ryz_tools_get_snapshot(ryz_tools_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    out->current_session = s_current;
    for (unsigned i = 0; i < RYZ_TOOL_COUNT; ++i) {
        out->tools[i].owner_session = s_slots[i].owner;
        out->tools[i].operation_id = s_slots[i].native_id;
        out->tools[i].closing = s_slots[i].closing;
        out->tools[i].cleanup_requested = s_slots[i].requested;
        out->tools[i].failed = s_slots[i].failed;
        out->tools[i].cleanup_error = s_slots[i].error;
    }
    leave();
    return ESP_OK;
}
esp_err_t ryz_tools_external(ryz_tool_t tool, esp_err_t (*admit)(void *), void *context)
{
    if ((unsigned)tool >= RYZ_TOOL_COUNT || !admit) return ESP_ERR_INVALID_ARG;
    if (!enter()) return ESP_ERR_NOT_FINISHED;
    esp_err_t error = s_slots[tool].owner ? ESP_ERR_NOT_FINISHED : admit(context);
    leave();
    return error;
}
