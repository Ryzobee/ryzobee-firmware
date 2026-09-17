#include "ryz_v5_font.h"
#include <limits.h>
#include <string.h>

typedef struct {
    uint32_t offset;
    uint16_t width, height;
    int16_t ofs_x, ofs_y;
    uint16_t advance;
} v5_glyph_t;

typedef struct {
    const v5_glyph_t *glyphs;
    const uint8_t *bitmap;
    unsigned count;
    uint32_t extra_codepoint;
} v5_font_data_t;

typedef struct {
    ryz_font_face_t face;
    unsigned px;
    const lv_font_t *font;
} v5_font_entry_t;

static unsigned extra_index(const v5_font_data_t *data, uint32_t cp)
{
    if (cp == data->extra_codepoint) return 95U;
    return UINT_MAX;
}

static esp_err_t failure;

static void fail(esp_err_t error)
{
    if (failure == ESP_OK) failure = error;
}

static bool glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                       uint32_t cp, uint32_t next)
{
    (void)next; /* Preserve the existing no-kerning layout. */
    if (!font || !font->dsc || !out) { fail(ESP_ERR_INVALID_ARG); return false; }
    *out = (lv_font_glyph_dsc_t){.resolved_font = font, .gid.index = cp};
    if (cp < 0x20) return true; /* Existing empty control/formatting glyphs. */
    const v5_font_data_t *data = font->dsc;
    unsigned index = cp >= 0x20 && cp <= 0x7e ? cp - 0x20 : extra_index(data, cp);
    if (index == UINT_MAX || index >= data->count) {
        fail(ESP_ERR_NOT_SUPPORTED);
        return false;
    }
    const v5_glyph_t *g = &data->glyphs[index];
    out->adv_w = g->advance;
    out->box_w = g->width;
    out->box_h = g->height;
    out->ofs_x = g->ofs_x;
    out->ofs_y = g->ofs_y;
    out->stride = g->width;
    out->format = LV_FONT_GLYPH_FORMAT_A8;
    return true;
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *buffer)
{
    if (!dsc || !dsc->resolved_font || !dsc->resolved_font->dsc) {
        fail(ESP_ERR_INVALID_ARG);
        return NULL;
    }
    const uint32_t cp = dsc->gid.index;
    if (cp < 0x20) return NULL;
    const v5_font_data_t *data = dsc->resolved_font->dsc;
    unsigned index = cp >= 0x20 && cp <= 0x7e ? cp - 0x20 : extra_index(data, cp);
    if (index == UINT_MAX || index >= data->count) {
        fail(ESP_ERR_NOT_SUPPORTED);
        return NULL;
    }
    const v5_glyph_t *g = &data->glyphs[index];
    if (dsc->box_w != g->width || dsc->box_h != g->height ||
        dsc->format != LV_FONT_GLYPH_FORMAT_A8) {
        fail(ESP_ERR_INVALID_ARG);
        return NULL;
    }
    if (!g->width || !g->height) return NULL;
    const uint8_t *bitmap = data->bitmap + g->offset;
    /* LVGL 9.5 unrotated A8 text blends directly from flash, with no glyph
     * cache lookup, decompression, rasterization or intermediate memcpy. */
    if (dsc->req_raw_bitmap) return bitmap;
    /* Keep the normal LVGL buffer contract for transformed text/diagnostics. */
    if (!buffer || !buffer->data || buffer->header.cf != LV_COLOR_FORMAT_A8) {
        fail(ESP_ERR_INVALID_ARG);
        return NULL;
    }
    const uint32_t stride = buffer->header.stride;
    if (stride < g->width || (size_t)stride * g->height > buffer->data_size ||
        !lv_draw_buf_reshape(buffer, LV_COLOR_FORMAT_A8, g->width, g->height, stride)) {
        fail(ESP_ERR_INVALID_SIZE);
        return NULL;
    }
    for (unsigned row = 0; row < g->height; ++row) {
        memcpy(buffer->data + row * stride, bitmap + row * g->width, g->width);
        memset(buffer->data + row * stride + g->width, 0, stride - g->width);
    }
    return buffer;
}

#include "ryz_v5_font_data.inc"

void ryz_v5_font_begin(void) { failure = ESP_OK; }
esp_err_t ryz_v5_font_status(void) { return failure; }

const lv_font_t *ryz_v5_font_get(ryz_font_face_t face, unsigned px)
{
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i)
        if (fonts[i].face == face && fonts[i].px == px) return fonts[i].font;
    fail(ESP_ERR_NOT_SUPPORTED);
    return NULL;
}
