#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    RYZ_FONT_BITMAP_GRAY = 0,
    RYZ_FONT_BITMAP_MONO,
} ryz_font_bitmap_mode_t;

typedef enum {
    RYZ_FONT_BITMAP_COPY_OK = 0,
    RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT,
    RYZ_FONT_BITMAP_COPY_DESTINATION_TOO_SMALL,
    RYZ_FONT_BITMAP_COPY_UNSUPPORTED_MODE,
} ryz_font_bitmap_copy_result_t;

/*
 * Copy a FreeType-style bitmap into tightly packed 8-bit alpha rows.
 * `source_buffer` points at the logical top row.  Adding the signed `pitch`
 * moves to the next logical row, exactly matching FT_Bitmap semantics.
 */
ryz_font_bitmap_copy_result_t ryz_font_bitmap_copy(
    const uint8_t *source_buffer,
    ptrdiff_t pitch,
    size_t width,
    size_t rows,
    ryz_font_bitmap_mode_t mode,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *out_required_bytes);
