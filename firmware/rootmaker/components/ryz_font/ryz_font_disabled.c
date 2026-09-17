/* Default build: no FreeType references, embedded TTFs or dynamic font heap.
 * init is a no-op; ready remains false. Static LVGL fonts need no engine. */
#include "ryz_font.h"
#include <string.h>

esp_err_t ryz_font_init(void) { return ESP_OK; }
bool ryz_font_ready(void) { return false; }
bool ryz_font_supports_codepoint(ryz_font_face_t face, uint32_t cp)
{ (void)face; (void)cp; return false; }
esp_err_t ryz_font_get_face_info(ryz_font_face_t face, ryz_font_face_info_t *out)
{
    (void)face;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ryz_font_get_metrics(ryz_font_face_t face, uint16_t px, ryz_font_metrics_t *out)
{
    (void)face; (void)px;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ryz_font_rasterize(ryz_font_face_t face, uint32_t cp, uint16_t px,
    uint8_t *bitmap, size_t capacity, ryz_font_glyph_t *out)
{
    (void)face; (void)cp; (void)px; (void)bitmap; (void)capacity;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ryz_font_rasterize_ascii(ryz_font_face_t face, uint32_t cp, uint16_t px,
    uint8_t *bitmap, size_t capacity, ryz_font_glyph_t *out)
{ return ryz_font_rasterize(face, cp, px, bitmap, capacity, out); }
