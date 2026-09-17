/* Static UI glyphs vs the production FreeType path retained for Lua.
 * No board timing claims: this exercises the exact native A8/metric contract. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ryz_v5_font.h"
#include "ryz_pixels_host.h"

#ifdef NDEBUG
#error "Static font checks require active assertions"
#endif

static const struct { ryz_font_face_t face; unsigned px; } palette[] = {
#define FONT(face, px) {face, px},
#include "ryz_v5_font_palette.def"
#undef FONT
};

static unsigned extra_count(ryz_font_face_t face)
{
    return (face == RYZ_FONT_DISPLAY || face == RYZ_FONT_BODY_SEMIBOLD || face == RYZ_FONT_BODY_MEDIUM ||
            face == RYZ_FONT_MONO_MEDIUM) ? 1U : 0U;
}

static uint32_t extra_codepoint(ryz_font_face_t face)
{
    return face == RYZ_FONT_DISPLAY || face == RYZ_FONT_BODY_SEMIBOLD ? RYZ_FONT_DEGREE_SIGN : RYZ_FONT_MIDDLE_DOT;
}

static void independent_static_path(void)
{
    assert(!ryz_font_ready());
    const size_t blocks = ryz_host_heap_blocks();
    ryz_host_heap_fail_after(0);
    ryz_v5_font_begin();
    for (unsigned i = 0; i < sizeof(palette) / sizeof(palette[0]); ++i) {
        const lv_font_t *font = ryz_v5_font_get(palette[i].face, palette[i].px);
        assert(font && font->static_bitmap && !font->fallback && !font->release_glyph);
        const unsigned count = 95 + extra_count(palette[i].face);
        for (unsigned c = 0; c < count; ++c) {
            uint32_t cp = c < 95 ? c + 0x20 : extra_codepoint(palette[i].face);
            lv_font_glyph_dsc_t g;
            assert(lv_font_get_glyph_dsc(font, &g, cp, 'V'));
            const void *a = lv_font_get_glyph_static_bitmap(&g);
            assert(!!a == !!(g.box_w && g.box_h));
            assert(a == lv_font_get_glyph_static_bitmap(&g));
        }
    }
    assert(ryz_v5_font_status() == ESP_OK && !ryz_font_ready());
    assert(ryz_host_heap_blocks() == blocks);
    ryz_host_heap_allow();
    puts("PASS all static glyphs before FreeType init, with allocation denied");
}

static void exact_glyphs(void)
{
    assert(ryz_font_init() == ESP_OK);
    unsigned checked = 0;
    size_t pixels = 0;
    for (unsigned i = 0; i < sizeof(palette) / sizeof(palette[0]); ++i) {
        const lv_font_t *font = ryz_v5_font_get(palette[i].face, palette[i].px);
        ryz_font_metrics_t metrics;
        assert(ryz_font_get_metrics(palette[i].face, palette[i].px, &metrics) == ESP_OK);
        assert(font->line_height == metrics.line_height && font->base_line == -metrics.descender);
        assert(font->kerning == LV_FONT_KERNING_NONE && font->subpx == LV_FONT_SUBPX_NONE);
        const unsigned count = 95 + extra_count(palette[i].face);
        for (unsigned c = 0; c < count; ++c) {
            uint8_t expected[8192];
            uint32_t cp = c < 95 ? c + 0x20 : extra_codepoint(palette[i].face);
            ryz_font_glyph_t old;
            assert(ryz_font_rasterize(palette[i].face, cp, palette[i].px,
                                      expected, sizeof(expected), &old) == ESP_OK);
            lv_font_glyph_dsc_t g;
            assert(lv_font_get_glyph_dsc(font, &g, cp, 'V'));
            assert(g.resolved_font == font && !g.is_placeholder && !g.entry);
            assert(g.adv_w == old.advance_x && g.box_w == old.width && g.box_h == old.height);
            assert(g.ofs_x == old.bearing_x && g.ofs_y == old.bearing_y - old.height);
            assert(g.stride == old.width && g.format == LV_FONT_GLYPH_FORMAT_A8);
            const uint8_t *raw = lv_font_get_glyph_static_bitmap(&g);
            if (old.bitmap_bytes) {
                assert(raw && !memcmp(raw, expected, old.bitmap_bytes));
                lv_draw_buf_t *buffer = lv_draw_buf_create(g.box_w, g.box_h,
                                                           LV_COLOR_FORMAT_A8, g.box_w + 5);
                assert(buffer);
                memset(buffer->data, 0xa5, buffer->data_size);
                assert(lv_font_get_glyph_bitmap(&g, buffer) == buffer);
                for (unsigned y = 0; y < g.box_h; ++y) {
                    assert(!memcmp(buffer->data + y * buffer->header.stride,
                                   expected + y * g.box_w, g.box_w));
                    for (unsigned x = g.box_w; x < buffer->header.stride; ++x)
                        assert(buffer->data[y * buffer->header.stride + x] == 0);
                }
                lv_draw_buf_destroy(buffer);
            } else assert(!raw);
            ++checked;
            pixels += old.bitmap_bytes;
        }
    }
    assert(ryz_v5_font_status() == ESP_OK);
    printf("PASS %u glyphs / %zu A8 pixels and all metrics match retained FreeType\n", checked, pixels);
}

static void rejection(void)
{
    const lv_font_t *font = ryz_v5_font_get(RYZ_FONT_BODY, 10);
    lv_font_glyph_dsc_t g;
    for (unsigned c = 0; c < 0x20; ++c) {
        assert(font->get_glyph_dsc(font, &g, c, 0));
        assert(!g.adv_w && !g.box_w && !g.box_h);
    }
    const uint32_t absent[] = {0x7f, 0xb0, 0x2103, 0x4e2d, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(absent) / sizeof(absent[0]); ++i) {
        ryz_v5_font_begin();
        assert(!font->get_glyph_dsc(font, &g, absent[i], 0));
        assert(ryz_v5_font_status() == ESP_ERR_NOT_SUPPORTED);
        assert(font->get_glyph_dsc(font, &g, 'A', 0));
        assert(ryz_v5_font_status() == ESP_ERR_NOT_SUPPORTED); /* Sticky in this render. */
    }
    ryz_v5_font_begin();
    const lv_font_t *degree_font=ryz_v5_font_get(RYZ_FONT_BODY_SEMIBOLD,14);
    assert(degree_font->get_glyph_dsc(degree_font,&g,RYZ_FONT_DEGREE_SIGN,0));
    assert(!degree_font->get_glyph_dsc(degree_font,&g,RYZ_FONT_MIDDLE_DOT,0));
    ryz_v5_font_begin();
    const lv_font_t *dot_font=ryz_v5_font_get(RYZ_FONT_BODY_MEDIUM,14);
    assert(dot_font->get_glyph_dsc(dot_font,&g,RYZ_FONT_MIDDLE_DOT,0));
    assert(!dot_font->get_glyph_dsc(dot_font,&g,RYZ_FONT_DEGREE_SIGN,0));
    ryz_v5_font_begin();
    assert(!ryz_v5_font_get(RYZ_FONT_DISPLAY, 19)); /* No dynamic fallback. */
    assert(!ryz_v5_font_get(RYZ_FONT_FACE_COUNT, 10));
    assert(ryz_v5_font_status() == ESP_ERR_NOT_SUPPORTED);
    ryz_v5_font_begin();
    assert(font->get_glyph_dsc(font, &g, 'A', 0));
    assert(!font->get_glyph_bitmap(&g, NULL));
    assert(ryz_v5_font_status() == ESP_ERR_INVALID_ARG);
    ryz_v5_font_begin();
    puts("PASS absent glyph/size rejected; no silent font fallback");
}

int main(void)
{
    lv_init();
    independent_static_path();
    exact_glyphs();
    rejection();
    puts("V5_STATIC_FONT_PASS");
    return 0;
}
