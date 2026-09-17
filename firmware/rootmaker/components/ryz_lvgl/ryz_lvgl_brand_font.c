#include "ryz_lvgl_brand_font.h"

#include <limits.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ASCII_GLYPH_COUNT (RYZ_FONT_ASCII_LAST - RYZ_FONT_ASCII_FIRST + 1U)
#define SUBSET_GLYPH_COUNT_MAX (ASCII_GLYPH_COUNT + 1U)
#define FONT_HEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static uint32_t extra_codepoint(ryz_font_face_t face, size_t ordinal)
{
    if (ordinal != 0U) return 0U;
    if (ryz_font_supports_codepoint(face, RYZ_FONT_DEGREE_SIGN))
        return RYZ_FONT_DEGREE_SIGN;
    if (ryz_font_supports_codepoint(face, RYZ_FONT_MIDDLE_DOT))
        return RYZ_FONT_MIDDLE_DOT;
    return 0U;
}

typedef struct {
    uint32_t codepoint;
    bool valid;
} glyph_cache_slot_t;

struct ryz_lvgl_brand_font {
    lv_font_t lv_font;
    TaskHandle_t owner;
    ryz_font_face_t face;
    uint16_t pixel_height;
    esp_err_t error;
    ryz_font_glyph_t metrics[SUBSET_GLYPH_COUNT_MAX];
    glyph_cache_slot_t slots[RYZ_LVGL_BRAND_FONT_CACHE_SLOTS_MAX];
    uint8_t *bitmaps;
    size_t slot_bytes;
    size_t slot_count;
    size_t next_slot;
};

static bool is_owner(const ryz_lvgl_brand_font_t *font)
{
    return font != NULL && font->owner == xTaskGetCurrentTaskHandle();
}

static void record_error(ryz_lvgl_brand_font_t *font, esp_err_t error)
{
    if (font->error == ESP_OK) font->error = error;
}

/* Keep one compact extension slot, not an array indexed by Unicode value. */
static int glyph_index(ryz_font_face_t face, uint32_t codepoint)
{
    if (!ryz_font_supports_codepoint(face, codepoint)) return -1;
    if (codepoint >= RYZ_FONT_ASCII_FIRST && codepoint <= RYZ_FONT_ASCII_LAST)
        return (int)(codepoint - RYZ_FONT_ASCII_FIRST);
    return (int)ASCII_GLYPH_COUNT;
}

static bool glyph_metrics_valid(const ryz_font_glyph_t *glyph)
{
    const int32_t ofs_y = (int32_t)glyph->bearing_y - glyph->height;
    return glyph->advance_x >= 0 && ofs_y >= INT16_MIN && ofs_y <= INT16_MAX &&
           glyph->bitmap_bytes == (size_t)glyph->width * glyph->height;
}

static bool glyph_metrics_equal(const ryz_font_glyph_t *left,
                                const ryz_font_glyph_t *right)
{
    return left->width == right->width && left->height == right->height &&
           left->bearing_x == right->bearing_x &&
           left->bearing_y == right->bearing_y &&
           left->advance_x == right->advance_x &&
           left->bitmap_bytes == right->bitmap_bytes;
}

static bool get_glyph_dsc(const lv_font_t *lv_font,
                          lv_font_glyph_dsc_t *out_dsc,
                          uint32_t codepoint,
                          uint32_t codepoint_next)
{
    (void)codepoint_next; /* This adapter does not implement pair kerning. */
    if (lv_font == NULL || lv_font->dsc == NULL) return false;
    ryz_lvgl_brand_font_t *font = (ryz_lvgl_brand_font_t *)lv_font->dsc;
    if (!is_owner(font)) return false;
    if (out_dsc == NULL) {
        record_error(font, ESP_ERR_INVALID_ARG);
        return false;
    }
    *out_dsc = (lv_font_glyph_dsc_t){0};
    const int index = glyph_index(font->face, codepoint);
    if (codepoint >= RYZ_FONT_ASCII_FIRST && index < 0) {
        record_error(font, ESP_ERR_NOT_SUPPORTED);
        return false;
    }
    out_dsc->resolved_font = lv_font;
    out_dsc->gid.index = codepoint;
    if (codepoint < RYZ_FONT_ASCII_FIRST) {
        out_dsc->format = LV_FONT_GLYPH_FORMAT_NONE;
        return true;
    }

    const ryz_font_glyph_t *glyph =
        &font->metrics[index];
    out_dsc->adv_w = (uint16_t)glyph->advance_x; /* LVGL 9.5: integer pixels. */
    out_dsc->box_w = glyph->width;
    out_dsc->box_h = glyph->height;
    out_dsc->ofs_x = glyph->bearing_x;
    out_dsc->ofs_y = (int16_t)((int32_t)glyph->bearing_y - glyph->height);
    out_dsc->stride = glyph->width;
    out_dsc->format = LV_FONT_GLYPH_FORMAT_A8;
    return true;
}

static const uint8_t *cached_bitmap(ryz_lvgl_brand_font_t *font,
                                   uint32_t codepoint,
                                   const ryz_font_glyph_t *expected)
{
    for (size_t index = 0; index < font->slot_count; ++index) {
        if (font->slots[index].valid &&
            font->slots[index].codepoint == codepoint) {
            return font->bitmaps + index * font->slot_bytes;
        }
    }

    const size_t index = font->next_slot;
    uint8_t *bitmap = font->bitmaps + index * font->slot_bytes;
    /* A failed rasterization must not leave a valid, partially overwritten hit. */
    font->slots[index].valid = false;
    ryz_font_glyph_t actual = {0};
    esp_err_t result = ryz_font_rasterize(
        font->face, codepoint, font->pixel_height, bitmap, font->slot_bytes,
        &actual);
    if (result == ESP_OK && !glyph_metrics_equal(expected, &actual)) {
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        record_error(font, result);
        return NULL;
    }
    font->slots[index] = (glyph_cache_slot_t){
        .codepoint = codepoint,
        .valid = true,
    };
    font->next_slot = (index + 1U) % font->slot_count;
    return bitmap;
}

static const void *get_glyph_bitmap(lv_font_glyph_dsc_t *dsc,
                                    lv_draw_buf_t *draw_buf)
{
    if (dsc == NULL || dsc->resolved_font == NULL ||
        dsc->resolved_font->dsc == NULL) {
        return NULL;
    }
    ryz_lvgl_brand_font_t *font =
        (ryz_lvgl_brand_font_t *)dsc->resolved_font->dsc;
    if (!is_owner(font)) return NULL;
    const uint32_t codepoint = dsc->gid.index;
    const int index = glyph_index(font->face, codepoint);
    if ((codepoint >= RYZ_FONT_ASCII_FIRST && index < 0) || dsc->req_raw_bitmap) {
        record_error(font, ESP_ERR_NOT_SUPPORTED);
        return NULL;
    }
    if (codepoint < RYZ_FONT_ASCII_FIRST) return NULL;
    const ryz_font_glyph_t *glyph =
        &font->metrics[index];
    if (dsc->box_w != glyph->width || dsc->box_h != glyph->height ||
        dsc->format != LV_FONT_GLYPH_FORMAT_A8) {
        record_error(font, ESP_ERR_INVALID_ARG);
        return NULL;
    }
    if (glyph->bitmap_bytes == 0) return NULL;
    if (draw_buf == NULL || draw_buf->data == NULL ||
        draw_buf->header.cf != LV_COLOR_FORMAT_A8) {
        record_error(font, ESP_ERR_INVALID_ARG);
        return NULL;
    }

    /* Preserve LVGL's row padding; the cache itself is tightly packed A8. */
    const uint32_t stride = draw_buf->header.stride;
    if (stride < glyph->width ||
        (size_t)stride * glyph->height > draw_buf->data_size ||
        lv_draw_buf_reshape(draw_buf, LV_COLOR_FORMAT_A8,
                            glyph->width, glyph->height, stride) == NULL) {
        record_error(font, ESP_ERR_INVALID_SIZE);
        return NULL;
    }
    const uint8_t *bitmap = cached_bitmap(font, codepoint, glyph);
    if (bitmap == NULL) return NULL;
    for (size_t row = 0; row < glyph->height; ++row) {
        uint8_t *destination = draw_buf->data + row * stride;
        memcpy(destination, bitmap + row * glyph->width, glyph->width);
        memset(destination + glyph->width, 0, stride - glyph->width);
    }
    return draw_buf;
}

esp_err_t ryz_lvgl_brand_font_create(ryz_font_face_t face,
                                    uint16_t pixel_height,
                                    ryz_lvgl_brand_font_t **out_font)
{
    if (out_font != NULL) *out_font = NULL;
    if (out_font == NULL || face < RYZ_FONT_BODY || face >= RYZ_FONT_FACE_COUNT ||
        pixel_height < RYZ_FONT_MIN_PIXEL_HEIGHT ||
        pixel_height > RYZ_FONT_MAX_PIXEL_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ryz_font_ready() || xTaskGetCurrentTaskHandle() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ryz_font_metrics_t face_metrics;
    esp_err_t result = ryz_font_get_metrics(face, pixel_height, &face_metrics);
    if (result != ESP_OK) return result;
    if (face_metrics.line_height <= 0 || face_metrics.descender > 0 ||
        face_metrics.descender == INT32_MIN ||
        -face_metrics.descender > face_metrics.line_height) {
        return ESP_ERR_INVALID_SIZE;
    }

    ryz_lvgl_brand_font_t *font =
        heap_caps_calloc(1, sizeof(*font), FONT_HEAP_CAPS);
    if (font == NULL) return ESP_ERR_NO_MEM;
    font->owner = xTaskGetCurrentTaskHandle();
    font->face = face;
    font->pixel_height = pixel_height;
    const size_t glyph_count = ASCII_GLYPH_COUNT +
        (ryz_font_supports_codepoint(face, RYZ_FONT_DEGREE_SIGN) ||
         ryz_font_supports_codepoint(face, RYZ_FONT_MIDDLE_DOT) ? 1U : 0U);
    for (size_t index = 0; index < glyph_count; ++index) {
        const uint32_t codepoint = index < ASCII_GLYPH_COUNT ?
            RYZ_FONT_ASCII_FIRST + (uint32_t)index : extra_codepoint(face, 0U);
        ryz_font_glyph_t *glyph = &font->metrics[index];
        result = ryz_font_rasterize(
            face, codepoint, pixel_height, NULL, 0, glyph);
        if (result != ESP_OK && result != ESP_ERR_INVALID_SIZE) goto fail;
        if (!glyph_metrics_valid(glyph) ||
            ((result == ESP_ERR_INVALID_SIZE) != (glyph->bitmap_bytes != 0))) {
            result = ESP_FAIL;
            goto fail;
        }
        if (glyph->bitmap_bytes > font->slot_bytes) {
            font->slot_bytes = glyph->bitmap_bytes;
        }
    }
    if (font->slot_bytes == 0 ||
        font->slot_bytes > RYZ_LVGL_BRAND_FONT_CACHE_BYTES_MAX /
                               RYZ_LVGL_BRAND_FONT_CACHE_SLOTS) {
        result = ESP_ERR_INVALID_SIZE;
        goto fail;
    }
    /* Divide before multiplying: every admitted face/size retains at least
     * the old eight slots, without making large fonts exceed the byte cap. */
    font->slot_count = RYZ_LVGL_BRAND_FONT_CACHE_BYTES_MAX / font->slot_bytes;
    if (font->slot_count > RYZ_LVGL_BRAND_FONT_CACHE_SLOTS_MAX) {
        font->slot_count = RYZ_LVGL_BRAND_FONT_CACHE_SLOTS_MAX;
    }
    font->bitmaps = heap_caps_calloc(
        font->slot_count, font->slot_bytes, FONT_HEAP_CAPS);
    if (font->bitmaps == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    font->lv_font = (lv_font_t){
        .get_glyph_dsc = get_glyph_dsc,
        .get_glyph_bitmap = get_glyph_bitmap,
        .line_height = face_metrics.line_height,
        .base_line = -face_metrics.descender,
        .subpx = LV_FONT_SUBPX_NONE,
        .kerning = LV_FONT_KERNING_NONE,
        .static_bitmap = 0,
        /* Conservative decoration defaults; no synthesized heavier glyphs. */
        .underline_position = -1,
        .underline_thickness = 1,
        .dsc = font,
    };
    *out_font = font;
    return ESP_OK;

fail:
    heap_caps_free(font->bitmaps);
    heap_caps_free(font);
    return result;
}

const lv_font_t *ryz_lvgl_brand_font_get(const ryz_lvgl_brand_font_t *font)
{
    return is_owner(font) ? &font->lv_font : NULL;
}

esp_err_t ryz_lvgl_brand_font_status(const ryz_lvgl_brand_font_t *font)
{
    if (font == NULL) return ESP_ERR_INVALID_ARG;
    return is_owner(font) ? font->error : ESP_ERR_INVALID_STATE;
}

void ryz_lvgl_brand_font_destroy(ryz_lvgl_brand_font_t *font)
{
    if (!is_owner(font)) return;
    heap_caps_free(font->bitmaps);
    heap_caps_free(font);
}
