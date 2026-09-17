#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freetype/freetype.h"
#include "lvgl.h"
#include "ryz_font.h"
#include "ryz_lvgl_brand_font.h"
#include "ryz_pixels_host.h"

#ifdef NDEBUG
#error "This diagnostic relies on active assertions; build without NDEBUG"
#endif
#if !defined(CONFIG_LV_TXT_ENC_UTF8) || defined(CONFIG_LV_TXT_ENC_ASCII)
#error "The native temperature label requires the actual target's LVGL UTF-8 configuration"
#endif

_Static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 5 &&
               LVGL_VERSION_PATCH == 0, "Update the audited LVGL contract before changing versions");
_Static_assert(LV_COLOR_DEPTH == 16 && LV_MEM_SIZE == 64 * 1024,
               "This baseline requires target RGB565/64KiB configuration");
_Static_assert(LV_DRAW_BUF_STRIDE_ALIGN == 1 && LV_DRAW_SW_DRAW_UNIT_CNT == 1 &&
               LV_USE_OS == LV_OS_NONE, "Target draw ownership/stride changed");

extern const uint8_t noto_start[] asm("_binary_noto_sans_regular_ascii_ttf_start");
extern const uint8_t noto_end[] asm("_binary_noto_sans_regular_ascii_ttf_end");
extern const uint8_t teko_start[] asm("_binary_teko_semibold_ascii_ttf_start");
extern const uint8_t teko_end[] asm("_binary_teko_semibold_ascii_ttf_end");
extern const uint8_t semibold_start[] asm("_binary_noto_sans_semibold_ascii_ttf_start");
extern const uint8_t semibold_end[] asm("_binary_noto_sans_semibold_ascii_ttf_end");
extern const uint8_t medium_start[] asm("_binary_noto_sans_medium_ascii_ttf_start");
extern const uint8_t medium_end[] asm("_binary_noto_sans_medium_ascii_ttf_end");
extern const uint8_t mono_start[] asm("_binary_roboto_mono_regular_ascii_ttf_start");
extern const uint8_t mono_end[] asm("_binary_roboto_mono_regular_ascii_ttf_end");
extern const uint8_t mono_medium_start[] asm("_binary_roboto_mono_medium_ascii_ttf_start");
extern const uint8_t mono_medium_end[] asm("_binary_roboto_mono_medium_ascii_ttf_end");
extern const uint8_t mono_semibold_start[] asm("_binary_roboto_mono_semibold_ascii_ttf_start");
extern const uint8_t mono_semibold_end[] asm("_binary_roboto_mono_semibold_ascii_ttf_end");
_Static_assert(RYZ_FONT_BODY == 0 && RYZ_FONT_DISPLAY == 1 &&
               RYZ_FONT_BODY_SEMIBOLD == 2 && RYZ_FONT_BODY_MEDIUM == 3 &&
               RYZ_FONT_MONO == 4 && RYZ_FONT_MONO_MEDIUM == 5 &&
               RYZ_FONT_MONO_SEMIBOLD == 6,
               "Embedded face IDs are append-only");
static FT_Library s_oracle_library;
static FT_Face s_oracle_faces[RYZ_FONT_FACE_COUNT];
static uint16_t s_frame[240 * 240];
static _Alignas(4) uint8_t s_draw[240 * 16 * 2];
static unsigned s_flushes, s_verified_glyphs, s_cases;
static size_t s_metadata_bytes;

static void passed(const char *name) { ++s_cases; printf("PASS %s\n", name); }
static ryz_lvgl_brand_font_t *make_font(ryz_font_face_t face, uint16_t size)
{
    ryz_lvgl_brand_font_t *font = NULL;
    assert(ryz_lvgl_brand_font_create(face, size, &font) == ESP_OK && font);
    return font;
}
static FT_Face oracle(ryz_font_face_t face, uint16_t size, uint32_t codepoint)
{
    FT_Face font = s_oracle_faces[face];
    assert(FT_Set_Pixel_Sizes(font, 0, size) == 0);
    assert(FT_Load_Char(font, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0);
    return font;
}
static void check_bitmap(FT_Face expected, lv_font_glyph_dsc_t *glyph)
{
    if (!glyph->box_w || !glyph->box_h) return;
    const uint32_t stride = glyph->box_w + 3;
    lv_draw_buf_t *buffer = lv_draw_buf_create(glyph->box_w, glyph->box_h,
                                              LV_COLOR_FORMAT_A8, stride);
    assert(buffer);
    memset(buffer->data, 0xa5, buffer->data_size);
    assert(lv_font_get_glyph_bitmap(glyph, buffer) == buffer);
    assert(buffer->header.cf == LV_COLOR_FORMAT_A8 && buffer->header.stride == stride);
    const FT_Bitmap *bitmap = &expected->glyph->bitmap;
    assert(bitmap->pixel_mode == FT_PIXEL_MODE_GRAY && bitmap->pitch > 0);
    for (uint32_t y = 0; y < glyph->box_h; ++y) {
        assert(memcmp(buffer->data + y * stride,
                      bitmap->buffer + y * (size_t)bitmap->pitch, glyph->box_w) == 0);
        for (uint32_t x = glyph->box_w; x < stride; ++x)
            assert(buffer->data[y * stride + x] == 0);
    }
    lv_font_glyph_release_draw_data(glyph);
    assert(glyph->entry == NULL);
    lv_draw_buf_destroy(buffer);
}
static void check_glyph(ryz_lvgl_brand_font_t *adapter, ryz_font_face_t face,
                        uint16_t size, uint32_t codepoint)
{
    FT_Face expected = oracle(face, size, codepoint);
    const lv_font_t *font = ryz_lvgl_brand_font_get(adapter);
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(font, &glyph, codepoint, 'V'));
    assert(glyph.resolved_font == font && glyph.entry == NULL && !glyph.is_placeholder);
    assert(glyph.adv_w == expected->glyph->advance.x / 64);
    assert(glyph.box_w == expected->glyph->bitmap.width);
    assert(glyph.box_h == expected->glyph->bitmap.rows);
    assert(glyph.ofs_x == expected->glyph->bitmap_left);
    assert(glyph.ofs_y == expected->glyph->bitmap_top - (int)glyph.box_h);
    assert(glyph.gid.index == codepoint);
    assert(!font->static_bitmap && !font->release_glyph && !font->fallback);
    assert(font->kerning == LV_FONT_KERNING_NONE);
    const int top = font->line_height - font->base_line - glyph.ofs_y - glyph.box_h;
    assert(top >= 0 && top + glyph.box_h <= font->line_height);
    check_bitmap(expected, &glyph);
    assert(ryz_lvgl_brand_font_status(adapter) == ESP_OK);
    ++s_verified_glyphs;
}

static void before_init(void)
{
    ryz_font_metrics_t metrics;
    ryz_lvgl_brand_font_t *font = (void *)(uintptr_t)1;
    assert(!ryz_font_ready());
    for (int face = 0; face < RYZ_FONT_FACE_COUNT; ++face) {
        for (uint32_t cp = 0x20; cp <= 0x7e; ++cp)
            assert(ryz_font_supports_codepoint(face, cp));
        assert(ryz_font_supports_codepoint(face, RYZ_FONT_DEGREE_SIGN) ==
               (face == RYZ_FONT_DISPLAY || face == RYZ_FONT_BODY_SEMIBOLD));
        assert(ryz_font_supports_codepoint(face, RYZ_FONT_MIDDLE_DOT) ==
               (face == RYZ_FONT_BODY_MEDIUM || face == RYZ_FONT_MONO_MEDIUM));
        assert(!ryz_font_supports_codepoint(face, 0x2103U));
        assert(!ryz_font_supports_codepoint(face, 0x1fU));
    }
    assert(!ryz_font_supports_codepoint((ryz_font_face_t)-1, 'A'));
    assert(!ryz_font_supports_codepoint(RYZ_FONT_FACE_COUNT, 'A'));
    assert(!ryz_font_ready() && !ryz_host_heap_blocks());
    assert(ryz_font_get_metrics(RYZ_FONT_BODY, 12, &metrics) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_brand_font_create(RYZ_FONT_BODY, 12, &font) == ESP_ERR_INVALID_STATE);
    assert(!font && !ryz_host_heap_blocks());
    assert(ryz_font_init() == ESP_OK && ryz_font_init() == ESP_OK);
    assert(FT_Init_FreeType(&s_oracle_library) == 0);
    int major, minor, patch;
    FT_Library_Version(s_oracle_library, &major, &minor, &patch);
    assert(major == 2 && minor == 14 && patch == 3);
    const uint8_t *starts[] = {noto_start, teko_start, semibold_start,
                               medium_start, mono_start, mono_medium_start, mono_semibold_start};
    const uint8_t *ends[] = {noto_end, teko_end, semibold_end,
                             medium_end, mono_end, mono_medium_end, mono_semibold_end};
    const uint16_t weights[] = {400, 600, 600, 500, 400, 500, 600};
    const size_t sizes[] = {9628, 9396, 9736, 9656, 12528, 12560, 12584};
    for (size_t i = 0; i < RYZ_FONT_FACE_COUNT; ++i) {
        assert(FT_New_Memory_Face(s_oracle_library, starts[i], ends[i] - starts[i],
                                  0, &s_oracle_faces[i]) == 0);
        ryz_font_face_info_t info;
        assert(ryz_font_get_face_info(i, &info) == ESP_OK);
        assert(info.weight == weights[i] && info.embedded_bytes == sizes[i]);
        assert(strcmp(info.family, s_oracle_faces[i]->family_name) == 0);
    }
    passed("initialization contract / real embedded TTF and FreeType 2.14.3");
}
static void metrics_and_pixels(void)
{
    const uint16_t information_sizes[] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 20, 22, 24, 32, 64};
    const uint16_t display_sizes[] = {8, 10, 11, 12, 14, 15, 16, 17, 18, 20, 22, 24, 32, 64};
    for (int face = 0; face < RYZ_FONT_FACE_COUNT; ++face) {
        const uint16_t *sizes = face == RYZ_FONT_DISPLAY ? display_sizes : information_sizes;
        const size_t count = face == RYZ_FONT_DISPLAY ?
            sizeof(display_sizes) / sizeof(display_sizes[0]) :
            sizeof(information_sizes) / sizeof(information_sizes[0]);
        for (size_t i = 0; i < count; ++i) {
            uint16_t size = sizes[i];
            /* V5 uses tiny information fonts, not Teko: its hinted '(' at
             * 7px extends above the independently rounded face line height. */
            ryz_font_metrics_t metrics;
            FT_Face expected = oracle(face, size, 'A');
            assert(ryz_font_get_metrics(face, size, &metrics) == ESP_OK);
            assert(metrics.ascender == expected->size->metrics.ascender / 64);
            assert(metrics.descender == expected->size->metrics.descender / 64);
            assert(metrics.line_height == expected->size->metrics.height / 64);
            ryz_lvgl_brand_font_t *font = make_font(face, size);
            const lv_font_t *lvfont = ryz_lvgl_brand_font_get(font);
            assert(lvfont->line_height == metrics.line_height);
            assert(lvfont->base_line == -metrics.descender);
            size_t bytes = ryz_host_heap_bytes();
            for (uint32_t c = 0x20; c <= 0x7e; ++c) check_glyph(font, face, size, c);
            assert(ryz_host_heap_bytes() == bytes);
            ryz_lvgl_brand_font_destroy(font);
            assert(!ryz_host_heap_bytes() && !ryz_host_heap_blocks());
        }
    }
    passed("95 glyphs x 116 face/size pairs: V5 information 6/7px, FT metrics, baseline, padded A8 bytes");
}
static void allocation_floor(void)
{
    /* The existing external heap failure seam observes metadata separately
     * from the bitmap allocation, without reading the opaque adapter. */
    assert(!ryz_host_heap_peak());
    ryz_host_heap_fail_after(1);
    ryz_lvgl_brand_font_t *font = (void *)(uintptr_t)1;
    assert(ryz_lvgl_brand_font_create(RYZ_FONT_BODY, 12, &font) == ESP_ERR_NO_MEM);
    ryz_host_heap_allow();
    assert(!font && !ryz_host_heap_bytes() && !ryz_host_heap_blocks());
    s_metadata_bytes = ryz_host_heap_peak();
    assert(s_metadata_bytes > 0);
    printf("FONT_METADATA_HOST_BYTES %zu\n", s_metadata_bytes);
    passed("bitmap allocation failure releases the separately measured metadata");
}
static void every_size_budget_and_pixels(void)
{
    unsigned pairs = 0, min_slots = 32, max_slots = 0;
    size_t largest_bitmap = 0;
    for (int face = 0; face < RYZ_FONT_FACE_COUNT; ++face) {
        for (uint16_t size = 6; size <= 64; ++size) {
            ryz_lvgl_brand_font_t *adapter = make_font(face, size);
            const lv_font_t *font = ryz_lvgl_brand_font_get(adapter);
            assert(ryz_host_heap_blocks() == 2);
            const size_t allocated = ryz_host_heap_bytes();
            assert(allocated > s_metadata_bytes);
            const size_t bitmap_bytes = allocated - s_metadata_bytes;
            assert(bitmap_bytes <= 32768);
            size_t largest_glyph = 0;
            const unsigned glyph_count =
                95U + (ryz_font_supports_codepoint(face, RYZ_FONT_DEGREE_SIGN) ||
                        ryz_font_supports_codepoint(face, RYZ_FONT_MIDDLE_DOT));
            for (unsigned index = 0; index < glyph_count; ++index) {
                const uint32_t cp = index < 95U ? 0x20U + index :
                    ryz_font_supports_codepoint(face, RYZ_FONT_DEGREE_SIGN) ?
                        RYZ_FONT_DEGREE_SIGN : RYZ_FONT_MIDDLE_DOT;
                assert(FT_Get_Char_Index(s_oracle_faces[face], cp) != 0);
                FT_Face expected = oracle(face, size, cp);
                lv_font_glyph_dsc_t glyph;
                assert(lv_font_get_glyph_dsc(font, &glyph, cp, 0));
                assert(glyph.box_w == expected->glyph->bitmap.width);
                assert(glyph.box_h == expected->glyph->bitmap.rows);
                assert(glyph.adv_w == expected->glyph->advance.x / 64);
                assert(glyph.ofs_x == expected->glyph->bitmap_left);
                assert(glyph.ofs_y == expected->glyph->bitmap_top - (int)glyph.box_h);
                size_t area = (size_t)expected->glyph->bitmap.width * expected->glyph->bitmap.rows;
                if (area > largest_glyph) largest_glyph = area;
                /* Tiny Teko face line metrics can round inside its hinted
                 * extents; compare actual FT pixels, do not change the font. */
                check_bitmap(expected, &glyph);
                ++s_verified_glyphs;
            }
            assert(largest_glyph && bitmap_bytes % largest_glyph == 0);
            const unsigned slots = (unsigned)(bitmap_bytes / largest_glyph);
            assert(slots >= 8 && slots <= 32);
            if (size == 12) assert(slots == 32);
            if (slots < min_slots) min_slots = slots;
            if (slots > max_slots) max_slots = slots;
            if (bitmap_bytes > largest_bitmap) largest_bitmap = bitmap_bytes;
            assert(ryz_host_heap_bytes() == allocated);
            assert(ryz_lvgl_brand_font_status(adapter) == ESP_OK);
            if (size == 12 || size == 64)
                printf("FONT_BUDGET face=%d px=%u slot_bytes=%zu slots=%u bitmap=%zu total=%zu old_bitmap=%zu\n",
                       face, size, largest_glyph, slots, bitmap_bytes, allocated, largest_glyph * 8);
            ryz_lvgl_brand_font_destroy(adapter);
            assert(!ryz_host_heap_bytes() && !ryz_host_heap_blocks());
            ++pairs;
        }
    }
    assert(pairs == 413 && min_slots < 32 && max_slots == 32);
    printf("FONT_RANGE pairs=%u slots=%u..%u max_bitmap=%zu\n",
           pairs, min_slots, max_slots, largest_bitmap);
    passed("all 7 faces x every 6..64px: ASCII plus Teko degree included in actual FT bitmap budget, <=32KiB / no glyph heap growth");
}
static void invalid_arguments(void)
{
    ryz_font_metrics_t metrics = {1, 2, 3};
    const uint16_t invalid_sizes[] = {0, 5, 65, UINT16_MAX};
    for (size_t i = 0; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++i) {
        ryz_lvgl_brand_font_t *font = (void *)(uintptr_t)1;
        assert(ryz_lvgl_brand_font_create(RYZ_FONT_BODY, invalid_sizes[i], &font) ==
               ESP_ERR_INVALID_ARG && !font);
        assert(ryz_font_get_metrics(RYZ_FONT_BODY, invalid_sizes[i], &metrics) ==
               ESP_ERR_INVALID_ARG);
        assert(!metrics.ascender && !metrics.descender && !metrics.line_height);
    }
    ryz_lvgl_brand_font_t *font = (void *)(uintptr_t)1;
    assert(ryz_lvgl_brand_font_create(RYZ_FONT_FACE_COUNT, 12, &font) == ESP_ERR_INVALID_ARG);
    assert(!font);
    assert(ryz_lvgl_brand_font_create(RYZ_FONT_BODY, 12, NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_font_get_metrics(RYZ_FONT_FACE_COUNT, 12, &metrics) == ESP_ERR_INVALID_ARG);
    assert(ryz_font_get_metrics(RYZ_FONT_BODY, 12, NULL) == ESP_ERR_INVALID_ARG);
    assert(!ryz_lvgl_brand_font_get(NULL));
    assert(ryz_lvgl_brand_font_status(NULL) == ESP_ERR_INVALID_ARG);
    ryz_lvgl_brand_font_destroy(NULL);
    assert(!ryz_host_heap_blocks());
    passed("invalid face / size / NULL arguments clear results without allocating");
}
static void cache_isolation(void)
{
    ryz_lvgl_brand_font_t *a = make_font(RYZ_FONT_BODY, 14);
    ryz_lvgl_brand_font_t *b = make_font(RYZ_FONT_DISPLAY, 24);
    lv_font_glyph_dsc_t delayed;
    assert(lv_font_get_glyph_dsc(ryz_lvgl_brand_font_get(a), &delayed, 'j', 0));
    size_t bytes = ryz_host_heap_bytes();
    for (unsigned repeat = 0; repeat < 5; ++repeat) {
        for (uint32_t c = 0x21; c <= 0x7e; ++c) {
            check_glyph(a, RYZ_FONT_BODY, 14, c);
            check_glyph(b, RYZ_FONT_DISPLAY, 24, c);
        }
        check_glyph(b, RYZ_FONT_DISPLAY, 24, RYZ_FONT_DEGREE_SIGN);
    }
    check_bitmap(oracle(RYZ_FONT_BODY, 14, 'j'), &delayed);
    assert(ryz_host_heap_bytes() == bytes);
    ryz_lvgl_brand_font_destroy(a);
    check_glyph(b, RYZ_FONT_DISPLAY, 24, 'Q');
    ryz_lvgl_brand_font_destroy(b);
    assert(!ryz_host_heap_bytes());
    passed("cache eviction / delayed descriptor / face and size isolation");
}
static void warmed_menu_glyphs(void)
{
    /* A menu-sized working set must stay usable without reentering the shared
     * FreeType lock. The oracle has its own real FT faces and does not use it. */
    const char text[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    for (int face = 0; face < RYZ_FONT_FACE_COUNT; ++face) {
        ryz_lvgl_brand_font_t *font = make_font(face, 12);
        for (size_t i = 0; i < sizeof(text) - 1; ++i)
            check_glyph(font, face, 12, (uint8_t)text[i]);
        const size_t bytes = ryz_host_heap_bytes();
        ryz_host_mutex_fail(true);
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            for (size_t i = 0; i < sizeof(text) - 1; ++i)
                check_glyph(font, face, 12, (uint8_t)text[i]);
        }
        ryz_host_mutex_fail(false);
        assert(ryz_host_heap_bytes() == bytes);
        ryz_lvgl_brand_font_destroy(font);
        assert(!ryz_host_heap_blocks());
    }
    passed("32 warmed menu glyphs retain real pixels with FreeType unavailable / all faces");
}
static void unsupported(void)
{
    ryz_lvgl_brand_font_t *adapter = make_font(RYZ_FONT_BODY, 12);
    const lv_font_t *font = ryz_lvgl_brand_font_get(adapter);
    lv_font_glyph_dsc_t glyph;
    for (uint32_t c = 0; c < 0x20; ++c) {
        memset(&glyph, 0xa5, sizeof(glyph));
        assert(lv_font_get_glyph_dsc(font, &glyph, c, 0));
        assert(!glyph.adv_w && !glyph.box_w && !glyph.box_h);
        assert(glyph.format == LV_FONT_GLYPH_FORMAT_NONE);
    }
    assert(lv_font_get_glyph_dsc(font, &glyph, ' ', 0));
    assert(glyph.adv_w && !glyph.box_w && !glyph.box_h);
    assert(ryz_lvgl_brand_font_status(adapter) == ESP_OK);
    const uint32_t missing[] = {0x7f, 0xb0, RYZ_FONT_MIDDLE_DOT, 0x2713, 0x4e2d, 0x1f41d};
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i) {
        assert(!lv_font_get_glyph_dsc(font, &glyph, missing[i], 0));
        assert(!glyph.resolved_font && glyph.is_placeholder);
    }
    assert(ryz_lvgl_brand_font_status(adapter) == ESP_ERR_NOT_SUPPORTED);
    assert(lv_font_get_glyph_dsc(font, &glyph, 'A', 0));
    assert(ryz_lvgl_brand_font_status(adapter) == ESP_ERR_NOT_SUPPORTED);
    ryz_lvgl_brand_font_destroy(adapter);
    passed("control / space / missing Unicode and sticky diagnostic");
}
static void degree_glyph(void)
{
    assert(RYZ_FONT_DEGREE_SIGN == 0x00b0U);
    assert(FT_Get_Char_Index(s_oracle_faces[RYZ_FONT_DISPLAY], RYZ_FONT_DEGREE_SIGN) != 0);
    for (uint16_t px = 8; px <= 64; ++px) {
        ryz_lvgl_brand_font_t *adapter = make_font(RYZ_FONT_DISPLAY, px);
        const size_t allocated = ryz_host_heap_bytes();
        lv_font_glyph_dsc_t glyph;
        const bool supported = lv_font_get_glyph_dsc(ryz_lvgl_brand_font_get(adapter), &glyph, RYZ_FONT_DEGREE_SIGN, 'C');
        if (!supported) fprintf(stderr, "TEKO_DEGREE expected=supported actual=missing cp=U+00B0 px=%u\n", px);
        assert(supported && !glyph.is_placeholder && glyph.box_w && glyph.box_h);
        check_glyph(adapter, RYZ_FONT_DISPLAY, px, RYZ_FONT_DEGREE_SIGN);
        check_glyph(adapter, RYZ_FONT_DISPLAY, px, '~'); /* Sparse slot cannot alias ASCII's last glyph. */
        check_glyph(adapter, RYZ_FONT_DISPLAY, px, RYZ_FONT_DEGREE_SIGN);
        ryz_host_mutex_fail(true);
        check_glyph(adapter, RYZ_FONT_DISPLAY, px, RYZ_FONT_DEGREE_SIGN);
        ryz_host_mutex_fail(false);
        assert(ryz_host_heap_bytes() == allocated);

        uint8_t pixels[4096];
        ryz_font_glyph_t actual = {0};
        assert(ryz_font_rasterize(RYZ_FONT_DISPLAY, RYZ_FONT_DEGREE_SIGN, px, NULL, 0, &actual) == ESP_ERR_INVALID_SIZE);
        assert(actual.bitmap_bytes && actual.bitmap_bytes <= sizeof(pixels));
        assert(ryz_font_rasterize(RYZ_FONT_DISPLAY, RYZ_FONT_DEGREE_SIGN, px, pixels, actual.bitmap_bytes - 1, &actual) == ESP_ERR_INVALID_SIZE);
        assert(ryz_font_rasterize(RYZ_FONT_DISPLAY, RYZ_FONT_DEGREE_SIGN, px, pixels, sizeof(pixels), &actual) == ESP_OK);
        const FT_Face expected = oracle(RYZ_FONT_DISPLAY, px, RYZ_FONT_DEGREE_SIGN);
        assert(actual.width == expected->glyph->bitmap.width && actual.height == expected->glyph->bitmap.rows);
        assert(actual.advance_x == expected->glyph->advance.x / 64);
        assert(actual.bearing_x == expected->glyph->bitmap_left && actual.bearing_y == expected->glyph->bitmap_top);
        for (unsigned y = 0; y < actual.height; ++y)
            assert(!memcmp(pixels + y * actual.width, expected->glyph->bitmap.buffer + y * expected->glyph->bitmap.pitch, actual.width));
        assert(ryz_lvgl_brand_font_status(adapter) == ESP_OK);
        ryz_lvgl_brand_font_destroy(adapter);
        assert(!ryz_host_heap_blocks());
    }
    passed("Teko degree at every 8..64px: native cmap, baseline/advance, padded A8 and public rasterizer match independent FreeType; warmed cache / ASCII isolation");
}
static void degree_boundaries(void)
{
    const uint32_t missing[] = {0x7fU, 0xb1U, 0x2103U, 0x4e2dU, 0x10ffffU, UINT32_MAX};
    for (int face = 0; face < RYZ_FONT_FACE_COUNT; ++face) {
        ryz_lvgl_brand_font_t *adapter = make_font(face, 24);
        const lv_font_t *font = ryz_lvgl_brand_font_get(adapter);
        uint8_t pixels[4096];
        memset(pixels, 0xa5, sizeof(pixels));
        ryz_font_glyph_t raster;
        lv_font_glyph_dsc_t glyph;
        assert(ryz_font_rasterize_ascii(face, RYZ_FONT_DEGREE_SIGN, 24, pixels, sizeof(pixels), &raster) == ESP_ERR_INVALID_ARG);
        if (face != RYZ_FONT_DISPLAY && face != RYZ_FONT_BODY_SEMIBOLD) {
            assert(!lv_font_get_glyph_dsc(font, &glyph, RYZ_FONT_DEGREE_SIGN, 0));
            assert(ryz_font_rasterize(face, RYZ_FONT_DEGREE_SIGN, 24, pixels, sizeof(pixels), &raster) == ESP_ERR_NOT_SUPPORTED);
            assert(FT_Get_Char_Index(s_oracle_faces[face], RYZ_FONT_DEGREE_SIGN) == 0);
        }
        for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i) {
            assert(!ryz_font_supports_codepoint(face, missing[i]));
            assert(!lv_font_get_glyph_dsc(font, &glyph, missing[i], 0));
            assert(ryz_font_rasterize(face, missing[i], 24, pixels, sizeof(pixels), &raster) == ESP_ERR_NOT_SUPPORTED);
        }
        for (size_t i = 0; i < sizeof(pixels); ++i) assert(pixels[i] == 0xa5);
        assert(ryz_lvgl_brand_font_status(adapter) == ESP_ERR_NOT_SUPPORTED);
        assert(lv_font_get_glyph_dsc(font, &glyph, face == RYZ_FONT_DISPLAY ? RYZ_FONT_DEGREE_SIGN : 'A', 0));
        assert(ryz_lvgl_brand_font_status(adapter) == ESP_ERR_NOT_SUPPORTED);
        ryz_lvgl_brand_font_destroy(adapter);
        assert(!ryz_host_heap_blocks());
    }
    passed("only Teko adds degree: six other faces and all U+2103/unknown Unicode reject; legacy ASCII stays strict with untouched output");
}
static void bad_buffers(void)
{
    ryz_lvgl_brand_font_t *adapter = make_font(RYZ_FONT_BODY, 18);
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(ryz_lvgl_brand_font_get(adapter), &glyph, 'W', 0));
    lv_draw_buf_t *buffer = lv_draw_buf_create(glyph.box_w, glyph.box_h,
                                              LV_COLOR_FORMAT_A8, glyph.box_w + 3);
    assert(buffer);
    memset(buffer->data, 0x96, buffer->data_size);
    size_t bytes = buffer->data_size;
    assert(!lv_font_get_glyph_bitmap(&glyph, NULL));
    buffer->data_size = glyph.box_w;
    assert(!lv_font_get_glyph_bitmap(&glyph, buffer));
    buffer->data_size = bytes;
    uint32_t stride = buffer->header.stride;
    buffer->header.stride = glyph.box_w - 1;
    assert(!lv_font_get_glyph_bitmap(&glyph, buffer));
    buffer->header.stride = stride;
    buffer->header.cf = LV_COLOR_FORMAT_RGB565;
    assert(!lv_font_get_glyph_bitmap(&glyph, buffer));
    buffer->header.cf = LV_COLOR_FORMAT_A8;
    for (size_t i = 0; i < bytes; ++i) assert(buffer->data[i] == 0x96);
    assert(ryz_lvgl_brand_font_status(adapter) != ESP_OK);
    assert(lv_font_get_glyph_bitmap(&glyph, buffer) == buffer);
    lv_draw_buf_destroy(buffer);
    ryz_lvgl_brand_font_destroy(adapter);
    passed("NULL / undersized / stride / format rejection without overwrite");
}
static void allocation_and_lock_failure(void)
{
    bool created = false;
    for (size_t fail = 0; fail < 16; ++fail) {
        ryz_lvgl_brand_font_t *font = (void *)(uintptr_t)1;
        ryz_host_heap_fail_after(fail);
        esp_err_t error = ryz_lvgl_brand_font_create(RYZ_FONT_BODY, 14, &font);
        ryz_host_heap_allow();
        if (error == ESP_OK) {
            ryz_lvgl_brand_font_destroy(font);
            created = true;
        } else {
            assert(error == ESP_ERR_NO_MEM && font == NULL);
        }
        assert(!ryz_host_heap_blocks() && !ryz_host_heap_bytes());
        if (created) break;
    }
    assert(created);
    ryz_host_mutex_fail(true);
    ryz_lvgl_brand_font_t *font = (void *)(uintptr_t)1;
    assert(ryz_lvgl_brand_font_create(RYZ_FONT_BODY, 12, &font) == ESP_ERR_TIMEOUT);
    assert(!font && !ryz_host_heap_blocks());
    ryz_host_mutex_fail(false);
    font = make_font(RYZ_FONT_BODY, 14);
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(ryz_lvgl_brand_font_get(font), &glyph, 'Q', 0));
    lv_draw_buf_t *buffer = lv_draw_buf_create(glyph.box_w, glyph.box_h,
                                              LV_COLOR_FORMAT_A8, 0);
    assert(buffer);
    ryz_host_mutex_fail(true);
    assert(!lv_font_get_glyph_bitmap(&glyph, buffer));
    assert(ryz_lvgl_brand_font_status(font) == ESP_ERR_TIMEOUT);
    ryz_host_mutex_fail(false);
    assert(lv_font_get_glyph_bitmap(&glyph, buffer) == buffer);
    assert(ryz_lvgl_brand_font_status(font) == ESP_ERR_TIMEOUT);
    /* The warmed glyph must be served without reentering FreeType. */
    ryz_host_mutex_fail(true);
    assert(lv_font_get_glyph_bitmap(&glyph, buffer) == buffer);
    ryz_host_mutex_fail(false);
    lv_draw_buf_destroy(buffer);
    ryz_lvgl_brand_font_destroy(font);
    assert(!ryz_host_heap_blocks());
    passed("all adapter allocation points / shared FT lock failure / recovery");
}
typedef struct {
    ryz_lvgl_brand_font_t *font;
    const lv_font_t *lvfont;
    lv_font_glyph_dsc_t glyph;
    lv_draw_buf_t *buffer;
} foreign_t;
static void *foreign_owner(void *argument)
{
    foreign_t *foreign = argument;
    assert(!ryz_lvgl_brand_font_get(foreign->font));
    assert(ryz_lvgl_brand_font_status(foreign->font) == ESP_ERR_INVALID_STATE);
    lv_font_glyph_dsc_t glyph = {0};
    assert(!foreign->lvfont->get_glyph_dsc(foreign->lvfont, &glyph, 'A', 0));
    assert(!foreign->lvfont->get_glyph_bitmap(&foreign->glyph, foreign->buffer));
    for (size_t i = 0; i < foreign->buffer->data_size; ++i)
        assert(foreign->buffer->data[i] == 0x87);
    ryz_lvgl_brand_font_destroy(foreign->font);
    return NULL;
}
static void ownership(void)
{
    ryz_lvgl_brand_font_t *font = make_font(RYZ_FONT_BODY, 14);
    foreign_t context = {.font = font, .lvfont = ryz_lvgl_brand_font_get(font)};
    assert(lv_font_get_glyph_dsc(context.lvfont, &context.glyph, 'A', 0));
    context.buffer = lv_draw_buf_create(context.glyph.box_w, context.glyph.box_h,
                                        LV_COLOR_FORMAT_A8, 0);
    assert(context.buffer);
    memset(context.buffer->data, 0x87, context.buffer->data_size);
    size_t bytes = ryz_host_heap_bytes();
    pthread_t thread;
    assert(pthread_create(&thread, NULL, foreign_owner, &context) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(ryz_host_heap_bytes() == bytes);
    assert(ryz_lvgl_brand_font_status(font) == ESP_OK);
    check_glyph(font, RYZ_FONT_BODY, 14, 'A');
    lv_draw_buf_destroy(context.buffer);
    ryz_lvgl_brand_font_destroy(font);
    passed("foreign owner cannot use or free the UI-owned font");
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 240 && area->y2 < 240);
    size_t width = (size_t)(area->x2 - area->x1 + 1);
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(&s_frame[y * 240 + area->x1], pixels, width * 2);
        pixels += width * 2;
    }
    ++s_flushes;
    lv_display_flush_ready(display);
}
static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int x, int y,
                        const char *text, uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    assert(object);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_text(object, text);
    return object;
}
static void warmed_frame(void)
{
    for (int face = 0; face < RYZ_FONT_FACE_COUNT; ++face) {
        ryz_lvgl_brand_font_t *font = make_font(face, 12);
        lv_display_t *display = lv_display_create(240, 240);
        assert(display);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, flush);
        lv_obj_t *screen = lv_screen_active();
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        label(screen, ryz_lvgl_brand_font_get(font), 8, 8, "ABCDEFGHIJKLMNOP", 0xff6a00);
        label(screen, ryz_lvgl_brand_font_get(font), 8, 40, "QRSTUVWXYZ012345", 0xffffff);
        lv_refr_now(display);
        assert(ryz_lvgl_brand_font_status(font) == ESP_OK);
        uint16_t first[240 * 240];
        memcpy(first, s_frame, sizeof(first));
        const size_t bytes = ryz_host_heap_bytes();
        ryz_host_mutex_fail(true);
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            const unsigned before = s_flushes;
            lv_obj_invalidate(screen);
            lv_refr_now(display);
            assert(s_flushes > before); /* Real repaint, not an idle timer. */
            assert(memcmp(first, s_frame, sizeof(first)) == 0);
            assert(ryz_lvgl_brand_font_status(font) == ESP_OK);
        }
        ryz_host_mutex_fail(false);
        assert(ryz_host_heap_bytes() == bytes);
        lv_display_delete(display);
        ryz_lvgl_brand_font_destroy(font);
        assert(!ryz_host_heap_blocks());
    }
    passed("7 real LVGL hot frames repaint identical RGB565 without FreeType / repeated release");
}
static void put_le(FILE *file, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) assert(fputc((value >> (i * 8)) & 0xff, file) != EOF);
}
static void save_bmp(const char *directory, const char *name)
{
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s.bmp", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite("BM", 1, 2, file) == 2);
    put_le(file, 54 + 240 * 240 * 3, 4); put_le(file, 0, 4); put_le(file, 54, 4);
    put_le(file, 40, 4); put_le(file, 240, 4); put_le(file, 240, 4);
    put_le(file, 1, 2); put_le(file, 24, 2);
    for (unsigned i = 0; i < 6; ++i) put_le(file, 0, 4);
    for (int y = 239; y >= 0; --y) {
        for (int x = 0; x < 240; ++x) {
            uint16_t color = s_frame[y * 240 + x];
            uint8_t b = color & 31, g = (color >> 5) & 63, r = color >> 11;
            assert(fputc((b << 3) | (b >> 2), file) != EOF);
            assert(fputc((g << 2) | (g >> 4), file) != EOF);
            assert(fputc((r << 3) | (r >> 2), file) != EOF);
        }
    }
    assert(fclose(file) == 0);
    printf("PIXEL_ARTIFACT %s\n", path);
    length = snprintf(path, sizeof(path), "%s/%s.rgb565", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    file = fopen(path, "wb");
    assert(file);
    for (size_t i = 0; i < 240 * 240; ++i) put_le(file, s_frame[i], 2);
    assert(fclose(file) == 0);
}
static void render(const char *directory)
{
    ryz_lvgl_brand_font_t *body = make_font(RYZ_FONT_BODY, 12);
    ryz_lvgl_brand_font_t *large = make_font(RYZ_FONT_BODY, 14);
    ryz_lvgl_brand_font_t *title = make_font(RYZ_FONT_DISPLAY, 24);
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    const lv_font_t *b = ryz_lvgl_brand_font_get(body), *l = ryz_lvgl_brand_font_get(large);
    const lv_font_t *t = ryz_lvgl_brand_font_get(title);
    label(screen, t, 8, 2, "RYZOBEE / FONT PROBE", 0xff6a00);
    label(screen, b, 8, 38, "Noto Sans / 12px / 400", 0xa0a0a0);
    label(screen, b, 8, 57, "AgjpQ_ 0123456789", 0xffffff);
    label(screen, b, 8, 84, "Noto Sans / 14px / 400", 0xa0a0a0);
    label(screen, l, 8, 104, "AgjpQ_ 0123456789", 0xffffff);
    label(screen, b, 8, 136, "Teko / 24px / 600", 0xa0a0a0);
    label(screen, t, 8, 153, "AgjpQ_ 0123456789", 0xff6a00);
    label(screen, b, 8, 204, "REAL LVGL / RGB565", 0xa0a0a0);
    lv_refr_now(display);
    assert(s_flushes >= 15);
    unsigned lit = 0;
    for (unsigned i = 0; i < 240 * 240; ++i) lit += s_frame[i] != 0;
    assert(lit > 1000);
    uint16_t first[240 * 240];
    memcpy(first, s_frame, sizeof(first));
    lv_obj_invalidate(screen);
    lv_refr_now(display);
    assert(memcmp(first, s_frame, sizeof(first)) == 0);
    assert(ryz_lvgl_brand_font_status(body) == ESP_OK);
    assert(ryz_lvgl_brand_font_status(large) == ESP_OK);
    assert(ryz_lvgl_brand_font_status(title) == ESP_OK);
    save_bmp(directory, "brand-font-240");
    lv_display_delete(display); /* References/draw tasks die before fonts. */
    ryz_lvgl_brand_font_destroy(title);
    ryz_lvgl_brand_font_destroy(large);
    ryz_lvgl_brand_font_destroy(body);
    assert(!ryz_host_heap_blocks());
    passed("production brand-font callbacks -> real LVGL -> deterministic 240x240 RGB565");
}
static void render_degree(const char *directory)
{
    ryz_lvgl_brand_font_t *body = make_font(RYZ_FONT_BODY, 12);
    ryz_lvgl_brand_font_t *normal = make_font(RYZ_FONT_DISPLAY, 28);
    ryz_lvgl_brand_font_t *large = make_font(RYZ_FONT_DISPLAY, 64);
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    label(screen, ryz_lvgl_brand_font_get(body), 8, 8, "TEKO NATIVE DEGREE / DEMO", 0xa0a0a0);
    label(screen, ryz_lvgl_brand_font_get(body), 8, 36, "28px / UTF-8 U+00B0 + C", 0xa0a0a0);
    const char temperature[] = "48\xc2\xb0" "C";
    lv_obj_t *small = label(screen, ryz_lvgl_brand_font_get(normal), 8, 55, temperature, 0xff6a00);
    label(screen, ryz_lvgl_brand_font_get(body), 8, 105, "64px / NATIVE OUTLINE", 0xa0a0a0);
    label(screen, ryz_lvgl_brand_font_get(large), 8, 122, temperature, 0xff6a00);
    assert(!strcmp(lv_label_get_text(small), temperature));
    const unsigned before = s_flushes;
    lv_refr_now(display);
    assert(s_flushes >= before + 15);
    if (ryz_lvgl_brand_font_status(normal) != ESP_OK || ryz_lvgl_brand_font_status(large) != ESP_OK)
        fprintf(stderr, "TEKO_UTF8_RENDER normal_error=%d large_error=%d\n",
                ryz_lvgl_brand_font_status(normal), ryz_lvgl_brand_font_status(large));
    assert(ryz_lvgl_brand_font_status(body) == ESP_OK);
    assert(ryz_lvgl_brand_font_status(normal) == ESP_OK);
    assert(ryz_lvgl_brand_font_status(large) == ESP_OK);
    const lv_font_t *font = ryz_lvgl_brand_font_get(normal);
    lv_font_glyph_dsc_t glyph;
    int degree_x = 8;
    assert(lv_font_get_glyph_dsc(font, &glyph, '4', '8')); degree_x += glyph.adv_w;
    assert(lv_font_get_glyph_dsc(font, &glyph, '8', RYZ_FONT_DEGREE_SIGN)); degree_x += glyph.adv_w;
    assert(lv_font_get_glyph_dsc(font, &glyph, RYZ_FONT_DEGREE_SIGN, 'C'));
    degree_x += glyph.ofs_x;
    const int degree_y = 55 + font->line_height - font->base_line - glyph.ofs_y - glyph.box_h;
    assert(degree_x >= 8 && degree_x + glyph.box_w < 240 && degree_y >= 55 && degree_y + glyph.box_h < 105);
    unsigned degree_ink = 0;
    for (unsigned y = 0; y < glyph.box_h; ++y) for (unsigned x = 0; x < glyph.box_w; ++x)
        degree_ink += s_frame[(degree_y + y) * 240 + degree_x + x] != 0;
    assert(degree_ink > 0);
    uint16_t first[240 * 240];
    memcpy(first, s_frame, sizeof(first));
    ryz_host_mutex_fail(true); /* UTF-8 degree must be a real reusable glyph, too. */
    lv_obj_invalidate(screen); lv_refr_now(display);
    ryz_host_mutex_fail(false);
    assert(!memcmp(first, s_frame, sizeof(first)));
    assert(ryz_lvgl_brand_font_status(body) == ESP_OK);
    assert(ryz_lvgl_brand_font_status(normal) == ESP_OK && ryz_lvgl_brand_font_status(large) == ESP_OK);
    save_bmp(directory, "brand-font-degree-240");
    lv_display_delete(display);
    ryz_lvgl_brand_font_destroy(large);
    ryz_lvgl_brand_font_destroy(normal);
    ryz_lvgl_brand_font_destroy(body);
    assert(!ryz_host_heap_blocks());
    passed("UTF-8 48-degree-C uses native Teko ink in a deterministic real LVGL 240px RGB565 frame / cached redraw");
}
int main(int argc, char **argv)
{
    assert(argc == 2 || (argc == 3 && (!strcmp(argv[2], "--cache") || !strcmp(argv[2], "--degree"))));
    before_init();
    lv_init();
    allocation_floor();
    if (argc == 3 && !strcmp(argv[2], "--degree")) {
        degree_glyph(); degree_boundaries(); render_degree(argv[1]);
    }
    else { warmed_menu_glyphs(); warmed_frame(); }
    if (argc == 2) {
        degree_glyph(); degree_boundaries();
        every_size_budget_and_pixels();
        invalid_arguments(); metrics_and_pixels(); cache_isolation(); unsupported(); bad_buffers();
        allocation_and_lock_failure(); ownership(); render(argv[1]); render_degree(argv[1]);
    }
    lv_deinit();
    for (unsigned i = 0; i < RYZ_FONT_FACE_COUNT; ++i)
        assert(FT_Done_Face(s_oracle_faces[i]) == 0);
    assert(FT_Done_FreeType(s_oracle_library) == 0);
    printf("BRAND_FONT_HOST_PASS cases=%u glyph_checks=%u adapter_peak=%zu live_bytes=%zu\n",
           s_cases, s_verified_glyphs, ryz_host_heap_peak(), ryz_host_heap_bytes());
    return 0;
}
