#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ryz_font_bitmap.h"

static void test_gray_rows_follow_signed_pitch(void)
{
    const uint8_t positive[] = {
        1, 2, 0xee,
        3, 4, 0xee,
        5, 6, 0xee,
    };
    uint8_t output[6] = {0};
    size_t required = 0;

    assert(ryz_font_bitmap_copy(positive,
                                3,
                                2,
                                3,
                                RYZ_FONT_BITMAP_GRAY,
                                output,
                                sizeof(output),
                                &required) == RYZ_FONT_BITMAP_COPY_OK);
    assert(required == sizeof(output));
    assert(memcmp(output, (uint8_t[]){1, 2, 3, 4, 5, 6}, sizeof(output)) == 0);

    /* FreeType points buffer at the logical top row.  A negative pitch means
     * adding pitch advances to the next logical row at a lower address. */
    const uint8_t negative_storage[] = {
        5, 6, 0xee,
        3, 4, 0xee,
        1, 2, 0xee,
    };
    memset(output, 0, sizeof(output));
    assert(ryz_font_bitmap_copy(negative_storage + 6,
                                -3,
                                2,
                                3,
                                RYZ_FONT_BITMAP_GRAY,
                                output,
                                sizeof(output),
                                &required) == RYZ_FONT_BITMAP_COPY_OK);
    assert(memcmp(output, (uint8_t[]){1, 2, 3, 4, 5, 6}, sizeof(output)) == 0);
}

static void test_mono_bitmap_expands_to_alpha(void)
{
    const uint8_t source[] = {0xa0, 0x40};
    uint8_t output[6] = {0};
    size_t required = 0;

    assert(ryz_font_bitmap_copy(source,
                                1,
                                3,
                                2,
                                RYZ_FONT_BITMAP_MONO,
                                output,
                                sizeof(output),
                                &required) == RYZ_FONT_BITMAP_COPY_OK);
    assert(required == sizeof(output));
    assert(memcmp(output,
                  (uint8_t[]){255, 0, 255, 0, 255, 0},
                  sizeof(output)) == 0);
}

static void test_capacity_query_reports_required_bytes(void)
{
    const uint8_t source[] = {1, 2, 3, 4, 5, 6};
    uint8_t too_small[5] = {0};
    size_t required = 0;

    assert(ryz_font_bitmap_copy(source,
                                3,
                                3,
                                2,
                                RYZ_FONT_BITMAP_GRAY,
                                NULL,
                                0,
                                &required) ==
           RYZ_FONT_BITMAP_COPY_DESTINATION_TOO_SMALL);
    assert(required == 6);

    required = 0;
    assert(ryz_font_bitmap_copy(source,
                                3,
                                3,
                                2,
                                RYZ_FONT_BITMAP_GRAY,
                                too_small,
                                sizeof(too_small),
                                &required) ==
           RYZ_FONT_BITMAP_COPY_DESTINATION_TOO_SMALL);
    assert(required == 6);
}

static void test_rejects_invalid_layout(void)
{
    const uint8_t source[] = {1, 2, 3, 4};
    uint8_t output[4] = {0};
    size_t required = 0;

    assert(ryz_font_bitmap_copy(source,
                                1,
                                2,
                                2,
                                RYZ_FONT_BITMAP_GRAY,
                                output,
                                sizeof(output),
                                &required) ==
           RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT);
    assert(required == sizeof(output));

    assert(ryz_font_bitmap_copy(source,
                                2,
                                2,
                                2,
                                (ryz_font_bitmap_mode_t)99,
                                output,
                                sizeof(output),
                                &required) ==
           RYZ_FONT_BITMAP_COPY_UNSUPPORTED_MODE);
}

int main(void)
{
    test_gray_rows_follow_signed_pitch();
    test_mono_bitmap_expands_to_alpha();
    test_capacity_query_reports_required_bytes();
    test_rejects_invalid_layout();
    puts("RYZ_FONT_BITMAP_PASS");
    return 0;
}
