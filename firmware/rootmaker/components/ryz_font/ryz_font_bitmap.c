#include "ryz_font_bitmap.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool pitch_magnitude(ptrdiff_t pitch, size_t *out_magnitude)
{
    if (out_magnitude == NULL || pitch == PTRDIFF_MIN) return false;
    *out_magnitude = (size_t)(pitch < 0 ? -pitch : pitch);
    return true;
}

ryz_font_bitmap_copy_result_t ryz_font_bitmap_copy(
    const uint8_t *source_buffer,
    ptrdiff_t pitch,
    size_t width,
    size_t rows,
    ryz_font_bitmap_mode_t mode,
    uint8_t *destination,
    size_t destination_capacity,
    size_t *out_required_bytes)
{
    if (out_required_bytes == NULL) {
        return RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT;
    }
    *out_required_bytes = 0;
    if (rows != 0 && width > SIZE_MAX / rows) {
        return RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT;
    }
    const size_t required = width * rows;
    *out_required_bytes = required;

    size_t source_row_bytes = 0;
    if (mode == RYZ_FONT_BITMAP_GRAY) {
        source_row_bytes = width;
    } else if (mode == RYZ_FONT_BITMAP_MONO) {
        if (width > SIZE_MAX - 7U) {
            return RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT;
        }
        source_row_bytes = (width + 7U) / 8U;
    } else {
        return RYZ_FONT_BITMAP_COPY_UNSUPPORTED_MODE;
    }

    size_t absolute_pitch = 0;
    if (!pitch_magnitude(pitch, &absolute_pitch) ||
        (rows > 0 && absolute_pitch < source_row_bytes) ||
        (rows > 1 && absolute_pitch != 0 &&
         rows - 1 > (size_t)PTRDIFF_MAX / absolute_pitch)) {
        return RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT;
    }
    if (required == 0) return RYZ_FONT_BITMAP_COPY_OK;
    if (source_buffer == NULL) {
        return RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT;
    }
    if (destination == NULL || destination_capacity < required) {
        return RYZ_FONT_BITMAP_COPY_DESTINATION_TOO_SMALL;
    }

    for (size_t row = 0; row < rows; ++row) {
        const ptrdiff_t row_offset = (ptrdiff_t)row * pitch;
        const uint8_t *pixels = source_buffer + row_offset;
        uint8_t *output = destination + row * width;
        if (mode == RYZ_FONT_BITMAP_GRAY) {
            memcpy(output, pixels, width);
            continue;
        }
        for (size_t column = 0; column < width; ++column) {
            output[column] =
                (pixels[column / 8U] & (0x80U >> (column % 8U))) ? 255U : 0U;
        }
    }
    return RYZ_FONT_BITMAP_COPY_OK;
}
