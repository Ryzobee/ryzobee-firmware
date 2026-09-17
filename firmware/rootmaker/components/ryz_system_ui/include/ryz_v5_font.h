#pragma once

#include "lvgl.h"
#include "ryz_font.h" /* Existing face identifiers only; no runtime FT calls. */

/* C system UI only. Immutable A8 glyphs/metrics live in flash for the finite
 * V5 palette. No allocation, FreeType initialization, cache or fallback.
 * Begin/get/callback/status run on the existing single UI Owner task.
 * Missing size/codepoint records NOT_SUPPORTED until the next begin(). */
void ryz_v5_font_begin(void);
const lv_font_t *ryz_v5_font_get(ryz_font_face_t face, unsigned px);
esp_err_t ryz_v5_font_status(void);
