#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Trusted C system-page Interface, not the public Lua scene model.
 *
 * There is one physical display and one exclusive Lua OR system lease. All
 * calls and all direct LVGL operations must run on the same UI Owner task;
 * this Interface adds neither a task nor an independent LVGL display. A Lua
 * lease rejects system calls, and a system lease rejects Lua mount/pump.
 * Lua release cannot release a system lease. Cross-owner calls never mutate
 * the lease or its objects. Callers must not re-enter this Interface from an
 * LVGL event, timer, deletion or cancellation callback.
 *
 * The caller may build/update LVGL children and styles on the borrowed root.
 * It must not delete/load/reparent that root, create another screen/display,
 * run lv_timer_handler/lv_refr_now, or retain it after commit failure/release.
 * System objects are bounded by the shared LVGL pool, not Lua's 32-object or
 * 40-byte text limits. Fonts, image bytes and callback data remain caller-owned
 * and must outlive every LVGL reference to them.
 */

/**
 * Acquire the system lease if unclaimed and create one 240x240 opaque black,
 * non-scrollable candidate root. *out_root is cleared on every failure.
 * A system lease may have one active root plus at most one candidate; another
 * create while a candidate exists returns INVALID_STATE. Allocation failure
 * while first acquiring the lease releases that lease; an existing lease and
 * active root remain intact. The bridge owns both roots, including candidates.
 * Creating does not present pixels or make the candidate interactive.
 * This handles allocation failures returned by LVGL, not arbitrary internal
 * LVGL pool exhaustion: some LVGL allocation sites assert or assume success.
 */
esp_err_t ryz_lvgl_system_create(lv_obj_t **out_root);

/**
 * Present the exact candidate returned by create, with checked synchronous
 * RGB565 transfer. A valid owner/candidate call ALWAYS consumes the candidate:
 * success makes it active and deletes the old root; failure deletes it and
 * restores the previous logical root (or idle root). The system lease remains
 * held on either result. Invalid/cross-owner calls leave all roots untouched.
 * Before committing, revoke caller-owned indev/timer/async references to the
 * previous root; establish such references to the new root only after success.
 *
 * Logical rollback does not repair partially transferred pixels. After any
 * failure the caller must revoke input/action eligibility and explicitly
 * repaint successfully before enabling it again. cancelled_out is optional,
 * always initialized false, and true only if this call actually observed its
 * cancellation callback returning true; a driver TIMEOUT is not cancellation.
 * The callback is synchronous, must not call LVGL/Lua, and is never retained.
 * Flush success does not certify caller-owned fonts or business state; check
 * their status and the navigation generation before publishing UI actions.
 */
esp_err_t ryz_lvgl_system_commit(lv_obj_t *candidate,
                                bool (*cancelled)(void *), void *context,
                                bool *cancelled_out);

/**
 * Advance LVGL timers/input/drawing only with a committed system root. Uses
 * the same cancellation/error contract as commit but retains the active root.
 * A pending candidate is not loaded by pump. A successful call need not have
 * transferred pixels when nothing was due; it is not an input-release guard.
 */
esp_err_t ryz_lvgl_system_pump(bool (*cancelled)(void *), void *context,
                              bool *cancelled_out);

/**
 * Caller MUST first disable/delete its indev, timers, async callbacks and any
 * other references capable of touching these roots. Also revoke the current
 * gesture/navigation token. Release deletes active and candidate roots, then
 * explicitly erases the entire retained draw buffer before freeing the lease,
 * without presenting the idle screen. It retains the shared display/buffer
 * allocation for the next lease. This erases only LVGL scratch pixels, not the
 * BSP canvas, DMA storage or physical panel; their privacy barrier is separate.
 * Ownerless release is an OK no-op and does not erase the buffer; a Lua lease
 * or another task's system lease returns INVALID_STATE untouched.
 * Release cannot be cancelled. External font/image storage may be freed only
 * after this call has returned and all other references have been removed.
 */
esp_err_t ryz_lvgl_system_release(void);

#ifdef __cplusplus
}
#endif
