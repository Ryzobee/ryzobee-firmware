/* Native, source-font-only audit. Link the project's managed FreeType.
 * This does not measure LVGL layout, device memory, or Unicode coverage beyond
 * the selected source cmap. The checksum is reproducible evidence, not a
 * cryptographic digest: every integer is serialized as little-endian u64,
 * followed by tightly packed top-to-bottom A8 pixels (no pitch padding). */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_FONT_FORMATS_H

#include "ryz_font_bitmap.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

typedef struct {
    unsigned weight, pixels;
    uint64_t codepoints, unique_glyphs, checksum;
    size_t max_bitmap_bytes;
    unsigned max_width, max_height;
    int64_t ascender, descender, height;
} probe_run_t;

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{ return (hash ^ value) * FNV_PRIME; }

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        hash = hash_byte(hash, (uint8_t)value);
        value >>= 8;
    }
    return hash;
}

static bool ft_ok(const char *operation, FT_Error error)
{
    if (!error) return true;
    fprintf(stderr, "unicode_font_probe: %s failed: FreeType error 0x%x\n",
            operation, (unsigned)error);
    return false;
}

static bool read_font(const char *path, unsigned char **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *bytes = NULL;
    bool ok = false;
    *out = NULL;
    *out_size = 0;
    if (!file) {
        fprintf(stderr, "unicode_font_probe: cannot open font: %s\n", strerror(errno));
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) goto io_error;
    long length = ftell(file);
    if (length < 4 || (uintmax_t)length > SIZE_MAX) {
        fprintf(stderr, "unicode_font_probe: invalid or unrepresentable font length\n");
        goto done;
    }
    if (fseek(file, 0, SEEK_SET) != 0) goto io_error;
    bytes = malloc((size_t)length);
    if (!bytes) {
        fprintf(stderr, "unicode_font_probe: font buffer allocation failed\n");
        goto done;
    }
    if (fread(bytes, 1, (size_t)length, file) != (size_t)length ||
        fgetc(file) != EOF || ferror(file)) goto io_error;
    /* A raw single-face TrueType sfnt, not WOFF/WOFF2, a TTC, or an OTF/CFF. */
    if (memcmp(bytes, "\0\1\0\0", 4) != 0 && memcmp(bytes, "true", 4) != 0) {
        fprintf(stderr, "unicode_font_probe: input is not a raw TrueType sfnt\n");
        goto done;
    }
    *out_size = (size_t)length;
    ok = true;
    goto done;
io_error:
    fprintf(stderr, "unicode_font_probe: font read/seek failed or file changed\n");
done:
    if (fclose(file) != 0) {
        fprintf(stderr, "unicode_font_probe: closing input failed\n");
        ok = false;
    }
    if (ok) *out = bytes;
    else { free(bytes); *out_size = 0; }
    return ok;
}

/* Validate the FreeType format, then exercise the production bitmap copier.
 * Only normalized 8-bit GRAY can be copied verbatim as firmware A8. */
static bool copy_bitmap(const FT_Bitmap *bitmap, uint8_t **copy,
                        size_t *capacity, size_t *required)
{
    const size_t width = bitmap->width, rows = bitmap->rows;
    if (rows && width > SIZE_MAX / rows) return false;
    *required = width * rows;
    ryz_font_bitmap_mode_t mode;
    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        if (*required && bitmap->num_grays != 256) return false;
        mode = RYZ_FONT_BITMAP_GRAY;
    } else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        mode = RYZ_FONT_BITMAP_MONO;
    } else return false;
    if (*required > *capacity) {
        uint8_t *larger = realloc(*copy, *required);
        if (!larger) return false;
        *copy = larger;
        *capacity = *required;
    }
    return ryz_font_bitmap_copy(bitmap->buffer, bitmap->pitch, width, rows,
                                mode, *copy, *capacity, required) == RYZ_FONT_BITMAP_COPY_OK;
}

static bool rasterize_cmap(FT_Face face, probe_run_t *run, uint8_t *seen,
                           size_t seen_size, uint8_t **copy, size_t *capacity)
{
    if (!ft_ok("FT_Set_Pixel_Sizes", FT_Set_Pixel_Sizes(face, 0, run->pixels))) return false;
    if (!face->size || face->size->metrics.height <= 0) {
        fprintf(stderr, "unicode_font_probe: invalid face size metrics\n");
        return false;
    }
    run->ascender = face->size->metrics.ascender;
    run->descender = face->size->metrics.descender;
    run->height = face->size->metrics.height;
    /* Exclude the requested weight: equal rendered metrics/pixels must hash
     * identically at the same size, even when hinting collapses two weights. */
    run->checksum = hash_u64(FNV_OFFSET, run->pixels);
    run->checksum = hash_u64(run->checksum, (uint64_t)run->ascender);
    run->checksum = hash_u64(run->checksum, (uint64_t)run->descender);
    run->checksum = hash_u64(run->checksum, (uint64_t)run->height);
    memset(seen, 0, seen_size);
    FT_UInt glyph;
    FT_ULong codepoint = FT_Get_First_Char(face, &glyph);
    while (glyph != 0) {
        if (codepoint > 0x10ffffUL || (codepoint >= 0xd800UL && codepoint <= 0xdfffUL) ||
            (size_t)glyph >= seen_size) {
            fprintf(stderr, "unicode_font_probe: invalid Unicode cmap mapping\n");
            return false;
        }
        FT_Error error = FT_Load_Glyph(face, glyph, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
        if (error) {
            fprintf(stderr, "unicode_font_probe: glyph render failed: weight=%u px=%u "
                    "codepoint=0x%lx gid=%u error=0x%x\n", run->weight, run->pixels,
                    (unsigned long)codepoint, glyph, (unsigned)error);
            return false;
        }
        FT_GlyphSlot slot = face->glyph;
        size_t required = 0;
        if (!slot || slot->format != FT_GLYPH_FORMAT_BITMAP ||
            !copy_bitmap(&slot->bitmap, copy, capacity, &required)) {
            fprintf(stderr, "unicode_font_probe: unsupported/unrepresentable bitmap or allocation failure: "
                    "weight=%u px=%u codepoint=0x%lx gid=%u\n",
                    run->weight, run->pixels, (unsigned long)codepoint, glyph);
            return false;
        }
        ++run->codepoints; /* Includes U+0000 if its mapped glyph is nonzero. */
        if (!seen[glyph]) { seen[glyph] = 1; ++run->unique_glyphs; }
        if (required > run->max_bitmap_bytes) run->max_bitmap_bytes = required;
        if (slot->bitmap.width > run->max_width) run->max_width = slot->bitmap.width;
        if (slot->bitmap.rows > run->max_height) run->max_height = slot->bitmap.rows;
        const int64_t values[] = {
            (int64_t)codepoint, glyph, slot->bitmap.width, slot->bitmap.rows,
            slot->bitmap_left, slot->bitmap_top, slot->advance.x, slot->advance.y,
            slot->metrics.width, slot->metrics.height, slot->metrics.horiBearingX,
            slot->metrics.horiBearingY, slot->metrics.horiAdvance,
            slot->metrics.vertBearingX, slot->metrics.vertBearingY, slot->metrics.vertAdvance,
        };
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            run->checksum = hash_u64(run->checksum, (uint64_t)values[i]);
        for (size_t i = 0; i < required; ++i) run->checksum = hash_byte(run->checksum, (*copy)[i]);
        FT_ULong next = FT_Get_Next_Char(face, codepoint, &glyph);
        if (glyph && next <= codepoint) {
            fprintf(stderr, "unicode_font_probe: cmap iteration did not advance\n");
            return false;
        }
        codepoint = next;
    }
    if (!run->codepoints) {
        fprintf(stderr, "unicode_font_probe: selected Unicode cmap is empty\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: unicode_font_probe RAW_TTF_PATH\n");
        return EXIT_FAILURE;
    }
    unsigned char *font = NULL;
    uint8_t *copy = NULL, *seen = NULL;
    FT_Fixed *coordinates = NULL;
    size_t font_bytes = 0, copy_capacity = 0;
    FT_Library library = NULL;
    FT_Face face = NULL;
    FT_MM_Var *variation = NULL;
    probe_run_t runs[9] = {0};
    const unsigned weights[] = {400, 500, 600}, sizes[] = {8, 12, 16};
    unsigned weight_axis = 0;
    unsigned axes = 0, units_per_em = 0;
    long face_glyphs = 0;
    int ft_major = 0, ft_minor = 0, ft_patch = 0;
    int64_t face_ascender = 0, face_descender = 0, face_height = 0;
    bool ok = false;
    if (!read_font(argv[1], &font, &font_bytes)) goto done;
    if (!ft_ok("FT_Init_FreeType", FT_Init_FreeType(&library))) goto done;
    FT_Library_Version(library, &ft_major, &ft_minor, &ft_patch);
    if (!ft_ok("FT_New_Memory_Face", FT_New_Memory_Face(library, font, (FT_Long)font_bytes, 0, &face))) goto done;
    const char *format = FT_Get_Font_Format(face);
    if (!FT_IS_SCALABLE(face) || !FT_IS_SFNT(face) || !FT_HAS_MULTIPLE_MASTERS(face) ||
        !format || strcmp(format, "TrueType") || (face->style_flags & FT_STYLE_FLAG_ITALIC) ||
        face->num_faces != 1 || face->num_glyphs <= 1 || (uintmax_t)face->num_glyphs > SIZE_MAX) {
        fprintf(stderr, "unicode_font_probe: expected a scalable, upright, variable TrueType face\n");
        goto done;
    }
    if (!ft_ok("FT_Select_Charmap", FT_Select_Charmap(face, FT_ENCODING_UNICODE))) goto done;
    if (!ft_ok("FT_Get_MM_Var", FT_Get_MM_Var(face, &variation))) goto done;
    if (!variation || !variation->num_axis || !variation->axis ||
        (size_t)variation->num_axis > SIZE_MAX / sizeof(*coordinates)) {
        fprintf(stderr, "unicode_font_probe: invalid variation axis table\n");
        goto done;
    }
    axes = variation->num_axis;
    coordinates = malloc((size_t)axes * sizeof(*coordinates));
    seen = calloc((size_t)face->num_glyphs, 1);
    if (!coordinates || !seen) {
        fprintf(stderr, "unicode_font_probe: coordinate/glyph bookkeeping allocation failed\n");
        goto done;
    }
    bool found_weight = false;
    for (unsigned i = 0; i < axes; ++i) {
        FT_Var_Axis *axis = &variation->axis[i];
        if (axis->minimum > axis->def || axis->def > axis->maximum ||
            ((axis->tag == FT_MAKE_TAG('i','t','a','l') || axis->tag == FT_MAKE_TAG('s','l','n','t')) && axis->def != 0)) {
            fprintf(stderr, "unicode_font_probe: invalid range or non-upright default axis %u\n", i);
            goto done;
        }
        if (axis->tag == FT_MAKE_TAG('w','g','h','t')) {
            if (found_weight || axis->minimum > 400L * 65536L || axis->maximum < 600L * 65536L) {
                fprintf(stderr, "unicode_font_probe: duplicate wght or missing 400..600 range\n");
                goto done;
            }
            found_weight = true;
            weight_axis = i;
        }
    }
    if (!found_weight) {
        fprintf(stderr, "unicode_font_probe: wght axis is absent\n");
        goto done;
    }
    face_glyphs = face->num_glyphs;
    units_per_em = face->units_per_EM;
    face_ascender = face->ascender;
    face_descender = face->descender;
    face_height = face->height;
    for (unsigned w = 0; w < 3; ++w) {
        for (unsigned i = 0; i < axes; ++i) coordinates[i] = variation->axis[i].def;
        coordinates[weight_axis] = (FT_Fixed)weights[w] * 65536L;
        if (!ft_ok("FT_Set_Var_Design_Coordinates", FT_Set_Var_Design_Coordinates(face, axes, coordinates)) ||
            !ft_ok("FT_Get_Var_Design_Coordinates", FT_Get_Var_Design_Coordinates(face, axes, coordinates))) goto done;
        for (unsigned i = 0; i < axes; ++i) {
            FT_Fixed expected = i == weight_axis ? (FT_Fixed)weights[w] * 65536L : variation->axis[i].def;
            if (coordinates[i] != expected) {
                fprintf(stderr, "unicode_font_probe: variation coordinate readback mismatch at axis %u\n", i);
                goto done;
            }
        }
        for (unsigned s = 0; s < 3; ++s) {
            probe_run_t *run = &runs[w * 3U + s];
            run->weight = weights[w];
            run->pixels = sizes[s];
            if (!rasterize_cmap(face, run, seen, (size_t)face_glyphs, &copy, &copy_capacity)) goto done;
            if (run->codepoints != runs[0].codepoints || run->unique_glyphs != runs[0].unique_glyphs) {
                fprintf(stderr, "unicode_font_probe: cmap population changed between runs\n");
                goto done;
            }
        }
    }
    ok = true;
done:
    free(copy);
    free(seen);
    free(coordinates);
    if (variation && !ft_ok("FT_Done_MM_Var", FT_Done_MM_Var(library, variation))) ok = false;
    if (face && !ft_ok("FT_Done_Face", FT_Done_Face(face))) ok = false;
    if (library && !ft_ok("FT_Done_FreeType", FT_Done_FreeType(library))) ok = false;
    free(font); /* FT_New_Memory_Face borrows these bytes until face teardown. */
    if (!ok) return EXIT_FAILURE;

    size_t maximum_bytes = 0;
    unsigned maximum_width = 0, maximum_height = 0;
    uint64_t checksum = FNV_OFFSET;
    for (unsigned i = 0; i < 9; ++i) {
        if (runs[i].max_bitmap_bytes > maximum_bytes) maximum_bytes = runs[i].max_bitmap_bytes;
        if (runs[i].max_width > maximum_width) maximum_width = runs[i].max_width;
        if (runs[i].max_height > maximum_height) maximum_height = runs[i].max_height;
        checksum = hash_u64(checksum, runs[i].checksum);
    }
    printf("{\"scope\":\"source_unicode_cmap_only\",\"font_bytes\":%zu,"
           "\"freetype_version\":\"%d.%d.%d\",\"scalable\":true,\"upright_metadata\":true,"
           "\"variable\":true,\"axis_count\":%u,\"codepoint_count\":%" PRIu64 ","
           "\"unique_glyph_count\":%" PRIu64 ",\"face_glyph_count\":%ld,"
           "\"weights\":[400,500,600],\"sizes\":[8,12,16],\"bitmap_format\":\"a8\","
           "\"max_bitmap_bytes\":%zu,\"max_bitmap_width\":%u,\"max_bitmap_height\":%u,"
           "\"face_metrics_units\":{\"units_per_em\":%u,\"ascender\":%" PRId64 ","
           "\"descender\":%" PRId64 ",\"height\":%" PRId64 "},"
           "\"checksum_algorithm\":\"fnv1a64-le-u64+row-major-a8-v1\","
           "\"checksum\":\"%016" PRIx64 "\",\"runs\":[",
           font_bytes, ft_major, ft_minor, ft_patch, axes, runs[0].codepoints,
           runs[0].unique_glyphs, face_glyphs, maximum_bytes, maximum_width, maximum_height,
           units_per_em, face_ascender, face_descender, face_height, checksum);
    for (unsigned i = 0; i < 9; ++i) {
        const probe_run_t *run = &runs[i];
        printf("%s{\"weight\":%u,\"size_px\":%u,\"codepoint_count\":%" PRIu64 ","
               "\"unique_glyph_count\":%" PRIu64 ",\"max_bitmap_bytes\":%zu,"
               "\"max_bitmap_width\":%u,\"max_bitmap_height\":%u,"
               "\"ascender_26_6\":%" PRId64 ",\"descender_26_6\":%" PRId64 ","
               "\"height_26_6\":%" PRId64 ",\"baseline_from_ascender_px\":%" PRId64 ","
               "\"checksum\":\"%016" PRIx64 "\"}", i ? "," : "", run->weight, run->pixels,
               run->codepoints, run->unique_glyphs, run->max_bitmap_bytes, run->max_width,
               run->max_height, run->ascender, run->descender, run->height,
               run->ascender / 64, run->checksum);
    }
    if (puts("]}") == EOF || fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "unicode_font_probe: writing summary failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
