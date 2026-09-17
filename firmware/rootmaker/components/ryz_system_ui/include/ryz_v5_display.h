#pragma once
#include "lvgl.h"
#include "ryz_display_settings.h"

typedef struct {
    esp_err_t (*get)(void *context, ryz_display_settings_snapshot_t *out);
    esp_err_t (*submit)(void *context, uint64_t revision, ryz_display_settings_value_t value);
    void *context;
    esp_err_t (*preview)(void *context, uint64_t revision, uint8_t brightness);
    void (*end_preview)(void *context);
} ryz_v5_display_binding_t;

/* Owner only. No hardware/NVS access, only snapshots and copied requests. */
void ryz_v5_display_bind(const ryz_v5_display_binding_t *binding);
/* Call on every route change, including entering DISPLAY. Discards local
 * draft/capture and releases temporary brightness; a submitted operation
 * still completes in the service and is never rolled back by this UI. */
void ryz_v5_display_reset(void);
/* Call while DISPLAY is current. True asks root to redraw. */
bool ryz_v5_display_tick(void);
/* Root draws the shared DISPLAY header. This draws y32..239 only. */
esp_err_t ryz_v5_display_draw(lv_obj_t *root);
/* Pass physical samples while current, including releases. NULL cancels
 * capture on read errors. Handles header Back and local discard confirmation;
 * back is cleared first, true only after a completed safe return action. */
bool ryz_v5_display_pointer(const ryz_touch_sample_t *sample, bool *back);
void ryz_v5_display_cancel_gesture(void);
