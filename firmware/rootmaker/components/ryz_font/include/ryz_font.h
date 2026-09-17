#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RYZ_FONT_ASCII_FIRST 0x20U
#define RYZ_FONT_ASCII_LAST 0x7eU
#define RYZ_FONT_DEGREE_SIGN 0x00b0U
#define RYZ_FONT_MIDDLE_DOT 0x00b7U
#define RYZ_FONT_MIN_PIXEL_HEIGHT 6U
#define RYZ_FONT_MAX_PIXEL_HEIGHT 64U

typedef enum {
    RYZ_FONT_BODY = 0,
    RYZ_FONT_DISPLAY,
    RYZ_FONT_BODY_SEMIBOLD,
    RYZ_FONT_BODY_MEDIUM,
    RYZ_FONT_MONO,
    RYZ_FONT_MONO_MEDIUM,
    RYZ_FONT_MONO_SEMIBOLD,
    RYZ_FONT_FACE_COUNT,
} ryz_font_face_t;

typedef struct {
    const char *family;
    uint16_t weight;
    size_t embedded_bytes;
} ryz_font_face_info_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t bearing_x;
    int16_t bearing_y;
    int16_t advance_x;
    size_t bitmap_bytes;
} ryz_font_glyph_t;

typedef struct {
    int32_t ascender;
    int32_t descender;
    int32_t line_height;
} ryz_font_metrics_t;

/** Initialize optional embedded faces. With CONFIG_RYZ_LUA_FREETYPE disabled,
 * init is an ESP_OK no-op, ready stays false, and raster/metadata APIs return
 * NOT_SUPPORTED. Static LVGL fonts have no runtime engine initialization. */
esp_err_t ryz_font_init(void);

/** Whether the shared FreeType library and all embedded faces are ready. */
bool ryz_font_ready(void);

/** Static subset coverage, available before init. Teko supports U+00B0 and the
 * V5 Medium faces support U+00B7. Celsius is encoded as degree + ASCII C, not
 * U+2103. */
bool ryz_font_supports_codepoint(ryz_font_face_t face, uint32_t codepoint);

/** Return immutable metadata for a face without exposing FreeType objects. */
esp_err_t ryz_font_get_face_info(ryz_font_face_t face,
                                 ryz_font_face_info_t *out_info);

/**
 * Return scaled face metrics in integer pixels under the shared FreeType lock.
 * Descender is signed (normally <= 0). line_height includes the face's line
 * spacing and is not necessarily pixel_height or ascender - descender.
 * On failure, a non-NULL output is cleared. Requires successful ryz_font_init.
 */
esp_err_t ryz_font_get_metrics(ryz_font_face_t face,
                               uint16_t pixel_height,
                               ryz_font_metrics_t *out_metrics);

/** Rasterize a supported subset code point with the same buffer/query
 * contract as rasterize_ascii. Unsupported code points return NOT_SUPPORTED;
 * no missing-glyph fallback is rendered. */
esp_err_t ryz_font_rasterize(ryz_font_face_t face,
                             uint32_t codepoint,
                             uint16_t pixel_height,
                             uint8_t *bitmap,
                             size_t bitmap_capacity,
                             ryz_font_glyph_t *out_glyph);

/**
 * Compatibility API: strictly printable ASCII; other code points continue
 * to return INVALID_ARG. Rasterize into a tightly packed 8-bit alpha
 * bitmap. The caller owns the output buffer; no FreeType pointer escapes.
 *
 * Pass `bitmap == NULL` and `bitmap_capacity == 0` to query the glyph metrics
 * and required `out_glyph->bitmap_bytes`.  A non-empty glyph returns
 * `ESP_ERR_INVALID_SIZE` for that query.  The same required byte count is
 * returned when a supplied buffer is too small, so callers can resize and
 * retry without guessing.
 */
esp_err_t ryz_font_rasterize_ascii(ryz_font_face_t face,
                                   uint32_t codepoint,
                                   uint16_t pixel_height,
                                   uint8_t *bitmap,
                                   size_t bitmap_capacity,
                                   ryz_font_glyph_t *out_glyph);

#ifdef __cplusplus
}
#endif
