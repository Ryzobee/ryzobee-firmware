#pragma once

#include "esp_err.h"
#include "ryz_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single-owner Adapter from the stable Ryzobee scene model to LVGL. The
 * caller must keep every call on the same task. No lv_obj_t escapes. A trusted
 * system-page lease rejects Lua mount/pump; Lua release never releases it.
 * Synchronous callback re-entry is rejected (void release becomes a no-op). */
esp_err_t ryz_lvgl_mount(const ryz_ui_scene_t *scene);
/* Update the mounted Lua scene in place. Scene id/generation, object order,
 * ids, kinds, geometry, font, align, radius and border width must match mount.
 * Text, colors, visible/enabled and scene background may change. No tree is
 * rebuilt and no caller text pointer is retained. Shared ryz_font_init must
 * have succeeded before Lua mount; body_16 is Noto Sans400/16px, title_24 is
 * Noto Sans600/24px, both printable ASCII.
 *
 * A refresh error/cancellation after mutation is NOT atomic rollback: pixels
 * may be partial and scene data already updated. The caller must stop input
 * and release/remount or terminate its Lua application; pump/update reject
 * this failed tree until a successful complete remount. Initial cancellation
 * and validation/admission rejection leave the prior scene untouched. */
esp_err_t ryz_lvgl_update(const ryz_ui_scene_t *scene);
esp_err_t ryz_lvgl_update_checked(const ryz_ui_scene_t *scene,
                                  bool (*cancelled)(void *), void *context);
esp_err_t ryz_lvgl_pump(void);
/* Cancellation is checked before work and between display stripes; an
 * in-flight DMA is drained before returning. The callback is owner-local,
 * must not re-enter LVGL or Lua, and is not retained after this call. A
 * cancelled refresh may require the caller to repaint its previous screen. */
esp_err_t ryz_lvgl_mount_checked(const ryz_ui_scene_t *scene,
                                 bool (*cancelled)(void *), void *context);
esp_err_t ryz_lvgl_pump_checked(bool (*cancelled)(void *), void *context);
void ryz_lvgl_release(void);

/* UI-owner-only access to official LVGL sysmon FPS, cached for 1000 ms.
 * Works with either active lease; never initializes LVGL or drives drawing.
 * False (and *fps = 0) before the first complete interval, with no lease,
 * or on cross-owner/reentrant calls. NULL is rejected. This is LVGL refresh
 * cycle FPS, not physical LCD scan rate or successful SPI commits. */
bool ryz_lvgl_get_fps(uint16_t *fps);

#ifdef __cplusplus
}
#endif
