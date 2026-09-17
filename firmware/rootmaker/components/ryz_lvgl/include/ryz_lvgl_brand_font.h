#pragma once

#include "lvgl.h"
#include "ryz_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Retained as the minimum; actual capacity is budgeted per face/size. */
#define RYZ_LVGL_BRAND_FONT_CACHE_SLOTS 8U
#define RYZ_LVGL_BRAND_FONT_CACHE_SLOTS_MAX 32U
#define RYZ_LVGL_BRAND_FONT_CACHE_BYTES_MAX (32U * 1024U)

typedef struct ryz_lvgl_brand_font ryz_lvgl_brand_font_t;

/**
 * Create a dynamic LVGL 9.5 A8 font backed by the shared ryz_font faces.
 * ryz_font_init must already have succeeded. The requested pixel height uses
 * ryz_font's 6..64 range; line height/baseline come from real face metrics.
 *
 * The instance preflights 95 printable-ASCII metrics (96 for Teko, including
 * U+00B0 degree), then allocates 8..32
 * fixed-size bitmap slots in PSRAM: as many as the largest glyph permits
 * within CACHE_BYTES_MAX, capped at CACHE_SLOTS_MAX. It never accepts fewer
 * than CACHE_SLOTS (the original minimum). Small menu fonts retain a larger
 * working set; large fonts retain the same 32 KiB bitmap limit and minimum
 * capacity. Sizes exceeding that minimum budget fail with INVALID_SIZE.
 * Metadata is additional, fixed-size storage. Allocation failures return
 * ESP_ERR_NO_MEM. Replacement is bounded FIFO, not a whole-ASCII cache.
 * No per-glyph allocation, FreeType face, or LVGL memory-pool allocation is
 * owned by this adapter. Every failure clears *out_font when it is non-NULL.
 *
 * Create, get, status, all LVGL glyph callbacks, and destroy must run on the
 * same UI Owner task. This also requires synchronous, single-owner LVGL draw
 * execution. The shared ryz_font layer serializes access by other clients.
 */
esp_err_t ryz_lvgl_brand_font_create(ryz_font_face_t face,
                                    uint16_t pixel_height,
                                    ryz_lvgl_brand_font_t **out_font);

/**
 * Borrow the LVGL font until destroy. Returns NULL for NULL/non-owner calls.
 * Printable ASCII is supported without kerning, plus U+00B0 for Teko only.
 * Controls U+0000..U+001F are empty formatting glyphs (including tab: zero
 * advance); other Unicode, including U+2103, is rejected and records
 * ESP_ERR_NOT_SUPPORTED. There is no silent Unicode fallback.
 *
 * Dynamic bitmap callbacks copy cached A8 pixels into LVGL's supplied draw
 * buffer, respecting its stride. They return that lv_draw_buf_t, never a cache
 * pointer. Empty glyphs have no bitmap. No glyph-release callback is required.
 */
const lv_font_t *ryz_lvgl_brand_font_get(const ryz_lvgl_brand_font_t *font);

/**
 * First owner-task callback error, sticky until destroy, or ESP_OK. Check after
 * measuring/rendering, because LVGL callbacks cannot return esp_err_t. A later
 * successful glyph does not clear an earlier error. NULL returns INVALID_ARG;
 * a non-owner call returns INVALID_STATE without touching the instance state.
 */
esp_err_t ryz_lvgl_brand_font_status(const ryz_lvgl_brand_font_t *font);

/**
 * Release adapter storage only, not the shared ryz_font library/faces. Before
 * calling, remove all LVGL object/style references to the borrowed lv_font_t
 * and finish pending drawing. NULL is allowed; non-owner calls do not free.
 */
void ryz_lvgl_brand_font_destroy(ryz_lvgl_brand_font_t *font);

#ifdef __cplusplus
}
#endif
