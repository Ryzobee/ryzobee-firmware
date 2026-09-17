#pragma once

#include "lvgl.h"
#include "ryz_font.h"

/* C-only V5 drawing helpers. Coordinates and pixel sizes come from the
 * original 240px Figma frames; this is not the bounded Lua scene schema. */
enum {
    V5_BLACK = 0x000000, V5_BG = 0x121212, V5_SURFACE = 0x181818,
    V5_SELECTED = 0x242424, V5_BORDER = 0x3A3A3A, V5_DISABLED = 0x666666,
    V5_MUTED = 0xA0A0A0, V5_BODY = 0xD9D9D9, V5_WHITE = 0xFFFFFF,
    V5_ORANGE = 0xFF6A00, V5_GREEN = 0x34C759, V5_RED = 0xFF453A,
    V5_WARNING = 0xFFB020,
};
void ryz_v5_widgets_begin(void);
/* Bounded current-page declaration cache. Only non-secret, side-effect-free
 * renderers opt in. A structural mismatch requires a fresh checked frame. */
esp_err_t ryz_v5_widgets_retain_begin(lv_obj_t *root, bool reuse);
esp_err_t ryz_v5_widgets_retain_end(void);
void ryz_v5_radius(lv_obj_t *object, int radius);
void ryz_v5_long_mode(lv_obj_t *object, lv_label_long_mode_t mode);
esp_err_t ryz_v5_widgets_status(void);
/* Call only after every system LVGL root using these fonts is released. */
void ryz_v5_widgets_release(void);
unsigned ryz_v5_widgets_font_count(void);
lv_obj_t *ryz_v5_box(lv_obj_t *root, int x, int y, int w, int h,
                     uint32_t bg, uint32_t border, int border_px);
lv_obj_t *ryz_v5_label(lv_obj_t *root, int x, int y, int w, int h,
                       const char *text, ryz_font_face_t face, unsigned px,
                       uint32_t color, lv_text_align_t align);
lv_obj_t *ryz_v5_image(lv_obj_t *root, int x, int y,
                       const lv_image_dsc_t *asset, uint32_t color);
/* Non-secret information fields only. NULL value reserves a private cell.
 * Pure measurement/draw: caller owns scroll offset; no LVGL indev or timers. */
typedef struct { const char *label, *value; } ryz_v5_info_field_t;
int ryz_v5_info_height(const ryz_v5_info_field_t *fields, size_t count);
int ryz_v5_info_row_y(const ryz_v5_info_field_t *fields, size_t index);
lv_obj_t *ryz_v5_info_draw(lv_obj_t *root, const ryz_v5_info_field_t *fields,
                          size_t count, int offset);
