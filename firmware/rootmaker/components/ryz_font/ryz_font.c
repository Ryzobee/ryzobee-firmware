#include "ryz_font.h"
#include "ryz_font_bitmap.h"

#include <limits.h>
#include <stdatomic.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freetype/freetype.h"
#include "freetype/ftmodapi.h"

#ifdef ESP_PLATFORM
#define FONT_MEMORY_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* Font parsing/rendering runs in ordinary tasks, never an ISR or DMA path.
 * Keep all FreeType allocations off the internal heap needed by task stacks
 * and Wi-Fi. The memory manager must outlive its library and all faces. */
static void *font_memory_alloc(FT_Memory memory, long bytes)
{
    (void)memory;
    return bytes > 0 ? heap_caps_malloc((size_t)bytes, FONT_MEMORY_CAPS) : NULL;
}

static void font_memory_free(FT_Memory memory, void *block)
{
    (void)memory;
    heap_caps_free(block);
}

static void *font_memory_realloc(FT_Memory memory, long old_bytes,
                               long new_bytes, void *block)
{
    (void)memory;
    (void)old_bytes;
    if (new_bytes <= 0) return NULL;
    /* On allocation failure the old block remains owned by FreeType. */
    return heap_caps_realloc(block, (size_t)new_bytes, FONT_MEMORY_CAPS);
}

static struct FT_MemoryRec_ s_font_memory = {
    .alloc = font_memory_alloc,
    .free = font_memory_free,
    .realloc = font_memory_realloc,
};
#endif

extern const uint8_t noto_sans_start[]
    asm("_binary_noto_sans_regular_ascii_ttf_start");
extern const uint8_t noto_sans_end[]
    asm("_binary_noto_sans_regular_ascii_ttf_end");
extern const uint8_t teko_start[]
    asm("_binary_teko_semibold_ascii_ttf_start");
extern const uint8_t teko_end[]
    asm("_binary_teko_semibold_ascii_ttf_end");
extern const uint8_t noto_sans_semibold_start[]
    asm("_binary_noto_sans_semibold_ascii_ttf_start");
extern const uint8_t noto_sans_semibold_end[]
    asm("_binary_noto_sans_semibold_ascii_ttf_end");
extern const uint8_t noto_sans_medium_start[]
    asm("_binary_noto_sans_medium_ascii_ttf_start");
extern const uint8_t noto_sans_medium_end[]
    asm("_binary_noto_sans_medium_ascii_ttf_end");
extern const uint8_t roboto_mono_start[]
    asm("_binary_roboto_mono_regular_ascii_ttf_start");
extern const uint8_t roboto_mono_end[]
    asm("_binary_roboto_mono_regular_ascii_ttf_end");
extern const uint8_t roboto_mono_medium_start[]
    asm("_binary_roboto_mono_medium_ascii_ttf_start");
extern const uint8_t roboto_mono_medium_end[]
    asm("_binary_roboto_mono_medium_ascii_ttf_end");
extern const uint8_t roboto_mono_semibold_start[]
    asm("_binary_roboto_mono_semibold_ascii_ttf_start");
extern const uint8_t roboto_mono_semibold_end[]
    asm("_binary_roboto_mono_semibold_ascii_ttf_end");

typedef struct {
    const char *family;
    uint16_t weight;
    const uint8_t *start;
    const uint8_t *end;
    FT_Face handle;
} embedded_face_t;

static const char *TAG = "ryz_font";
static FT_Library s_library;
static SemaphoreHandle_t s_lock;

typedef enum {
    FONT_INIT_UNINITIALIZED = 0,
    FONT_INIT_INITIALIZING,
    FONT_INIT_READY,
    FONT_INIT_FAILED,
} font_init_state_t;
static atomic_int s_init_state = ATOMIC_VAR_INIT(FONT_INIT_UNINITIALIZED);
static embedded_face_t s_faces[RYZ_FONT_FACE_COUNT] = {
    [RYZ_FONT_BODY] = {
        .family = "Noto Sans",
        .weight = 400,
        .start = noto_sans_start,
        .end = noto_sans_end,
    },
    [RYZ_FONT_DISPLAY] = {
        .family = "Teko",
        .weight = 600,
        .start = teko_start,
        .end = teko_end,
    },
    [RYZ_FONT_BODY_SEMIBOLD] = {
        .family = "Noto Sans",
        .weight = 600,
        .start = noto_sans_semibold_start,
        .end = noto_sans_semibold_end,
    },
    [RYZ_FONT_BODY_MEDIUM] = {
        .family = "Noto Sans",
        .weight = 500,
        .start = noto_sans_medium_start,
        .end = noto_sans_medium_end,
    },
    [RYZ_FONT_MONO] = {
        .family = "Roboto Mono",
        .weight = 400,
        .start = roboto_mono_start,
        .end = roboto_mono_end,
    },
    [RYZ_FONT_MONO_MEDIUM] = {
        .family = "Roboto Mono",
        .weight = 500,
        .start = roboto_mono_medium_start,
        .end = roboto_mono_medium_end,
    },
    [RYZ_FONT_MONO_SEMIBOLD] = {
        .family = "Roboto Mono",
        .weight = 600,
        .start = roboto_mono_semibold_start,
        .end = roboto_mono_semibold_end,
    },
};

static bool valid_face(ryz_font_face_t face)
{
    return face >= RYZ_FONT_BODY && face < RYZ_FONT_FACE_COUNT;
}

bool ryz_font_supports_codepoint(ryz_font_face_t face, uint32_t codepoint)
{
    return valid_face(face) &&
        ((codepoint >= RYZ_FONT_ASCII_FIRST && codepoint <= RYZ_FONT_ASCII_LAST) ||
         ((face == RYZ_FONT_DISPLAY || face == RYZ_FONT_BODY_SEMIBOLD) &&
          codepoint == RYZ_FONT_DEGREE_SIGN) ||
         ((face == RYZ_FONT_BODY_MEDIUM || face == RYZ_FONT_MONO_MEDIUM) &&
          codepoint == RYZ_FONT_MIDDLE_DOT));
}

static size_t embedded_size(const embedded_face_t *face)
{
    return (size_t)(face->end - face->start);
}

esp_err_t ryz_font_init(void)
{
    for (;;) {
        int state = atomic_load_explicit(&s_init_state, memory_order_acquire);
        if (state == FONT_INIT_READY) return ESP_OK;
        if (state == FONT_INIT_INITIALIZING) {
            taskYIELD();
            continue;
        }
        int expected = state;
        if (atomic_compare_exchange_weak_explicit(&s_init_state,
                                                  &expected,
                                                  FONT_INIT_INITIALIZING,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        atomic_store_explicit(
            &s_init_state, FONT_INIT_FAILED, memory_order_release);
        return ESP_ERR_NO_MEM;
    }

#ifdef ESP_PLATFORM
    s_library = NULL;
    FT_Error error = FT_New_Library(&s_font_memory, &s_library);
    if (error != FT_Err_Ok) goto fail;
    FT_Add_Default_Modules(s_library);
    FT_Set_Default_Properties(s_library);
#else
    /* The native pixel harness keeps FreeType's system allocator; its tracked
     * ESP heap models LVGL adapter lifetimes, not FreeType's shared caches.
     * PSRAM routing is a target-only property, not a Host validation claim. */
    FT_Error error = FT_Init_FreeType(&s_library);
    if (error != FT_Err_Ok) goto fail;
#endif

    for (size_t index = 0; index < RYZ_FONT_FACE_COUNT; ++index) {
        embedded_face_t *face = &s_faces[index];
        const size_t bytes = embedded_size(face);
        if (bytes == 0 || bytes > LONG_MAX) {
            error = FT_Err_Invalid_File_Format;
            goto fail;
        }
        error = FT_New_Memory_Face(
            s_library, face->start, (FT_Long)bytes, 0, &face->handle);
        if (error != FT_Err_Ok) goto fail;
        error = FT_Select_Charmap(face->handle, FT_ENCODING_UNICODE);
        if (error != FT_Err_Ok) goto fail;
    }
    atomic_store_explicit(&s_init_state, FONT_INIT_READY, memory_order_release);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "FreeType initialization failed: 0x%x", (unsigned)error);
    for (size_t index = 0; index < RYZ_FONT_FACE_COUNT; ++index) {
        if (s_faces[index].handle != NULL) {
            FT_Done_Face(s_faces[index].handle);
            s_faces[index].handle = NULL;
        }
    }
    if (s_library != NULL) {
#ifdef ESP_PLATFORM
        FT_Done_Library(s_library);
#else
        FT_Done_FreeType(s_library);
#endif
    }
    s_library = NULL;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    atomic_store_explicit(&s_init_state, FONT_INIT_FAILED, memory_order_release);
    return error == FT_Err_Out_Of_Memory ? ESP_ERR_NO_MEM : ESP_FAIL;
}

bool ryz_font_ready(void)
{
    return atomic_load_explicit(&s_init_state, memory_order_acquire) ==
           FONT_INIT_READY;
}

esp_err_t ryz_font_get_face_info(ryz_font_face_t face,
                                 ryz_font_face_info_t *out_info)
{
    if (!valid_face(face) || out_info == NULL) return ESP_ERR_INVALID_ARG;
    if (!ryz_font_ready()) return ESP_ERR_INVALID_STATE;
    const embedded_face_t *embedded = &s_faces[face];
    *out_info = (ryz_font_face_info_t){
        .family = embedded->family,
        .weight = embedded->weight,
        .embedded_bytes = embedded_size(embedded),
    };
    return ESP_OK;
}

esp_err_t ryz_font_get_metrics(ryz_font_face_t face,
                               uint16_t pixel_height,
                               ryz_font_metrics_t *out_metrics)
{
    if (out_metrics != NULL) *out_metrics = (ryz_font_metrics_t){0};
    if (!valid_face(face) || out_metrics == NULL ||
        pixel_height < RYZ_FONT_MIN_PIXEL_HEIGHT ||
        pixel_height > RYZ_FONT_MAX_PIXEL_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ryz_font_ready()) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    FT_Face handle = s_faces[face].handle;
    FT_Error error = FT_Set_Pixel_Sizes(handle, 0, pixel_height);
    esp_err_t result = ESP_OK;
    if (error != FT_Err_Ok) {
        result = error == FT_Err_Out_Of_Memory ? ESP_ERR_NO_MEM : ESP_FAIL;
    } else {
        /* FT_Size_Metrics already rounds these three 26.6 values to pixels. */
        const FT_Pos ascender = handle->size->metrics.ascender / 64;
        const FT_Pos descender = handle->size->metrics.descender / 64;
        const FT_Pos line_height = handle->size->metrics.height / 64;
        if (ascender < INT32_MIN || ascender > INT32_MAX ||
            descender < INT32_MIN || descender > INT32_MAX ||
            line_height <= 0 || line_height > INT32_MAX) {
            result = ESP_ERR_INVALID_SIZE;
        } else {
            *out_metrics = (ryz_font_metrics_t){
                .ascender = (int32_t)ascender,
                .descender = (int32_t)descender,
                .line_height = (int32_t)line_height,
            };
        }
    }
    xSemaphoreGive(s_lock);
    return result;
}

static esp_err_t copy_gray_bitmap(const FT_Bitmap *source,
                                  uint8_t *destination,
                                  size_t destination_size,
                                  size_t *out_required_bytes)
{
    ryz_font_bitmap_mode_t mode;
    if (source->pixel_mode == FT_PIXEL_MODE_GRAY) {
        mode = RYZ_FONT_BITMAP_GRAY;
    } else if (source->pixel_mode == FT_PIXEL_MODE_MONO) {
        mode = RYZ_FONT_BITMAP_MONO;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    switch (ryz_font_bitmap_copy(source->buffer,
                                 (ptrdiff_t)source->pitch,
                                 source->width,
                                 source->rows,
                                 mode,
                                 destination,
                                 destination_size,
                                 out_required_bytes)) {
    case RYZ_FONT_BITMAP_COPY_OK:
        return ESP_OK;
    case RYZ_FONT_BITMAP_COPY_DESTINATION_TOO_SMALL:
        return ESP_ERR_INVALID_SIZE;
    case RYZ_FONT_BITMAP_COPY_UNSUPPORTED_MODE:
        return ESP_ERR_NOT_SUPPORTED;
    case RYZ_FONT_BITMAP_COPY_INVALID_ARGUMENT:
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ryz_font_rasterize(ryz_font_face_t face,
                             uint32_t codepoint,
                             uint16_t pixel_height,
                             uint8_t *bitmap,
                             size_t bitmap_capacity,
                             ryz_font_glyph_t *out_glyph)
{
    if (!valid_face(face) ||
        pixel_height < RYZ_FONT_MIN_PIXEL_HEIGHT ||
        pixel_height > RYZ_FONT_MAX_PIXEL_HEIGHT || out_glyph == NULL ||
        (bitmap == NULL && bitmap_capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ryz_font_supports_codepoint(face, codepoint)) return ESP_ERR_NOT_SUPPORTED;
    if (!ryz_font_ready()) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    FT_Face handle = s_faces[face].handle;
    if (FT_Get_Char_Index(handle, codepoint) == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    FT_Error error = FT_Set_Pixel_Sizes(handle, 0, pixel_height);
    if (error == FT_Err_Ok) {
        error = FT_Load_Char(
            handle, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
    }
    if (error != FT_Err_Ok) {
        xSemaphoreGive(s_lock);
        return error == FT_Err_Out_Of_Memory ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    const FT_GlyphSlot slot = handle->glyph;
    const size_t bitmap_bytes = (size_t)slot->bitmap.width * slot->bitmap.rows;
    ryz_font_glyph_t glyph = {
        .width = (uint16_t)slot->bitmap.width,
        .height = (uint16_t)slot->bitmap.rows,
        .bearing_x = (int16_t)slot->bitmap_left,
        .bearing_y = (int16_t)slot->bitmap_top,
        .advance_x = (int16_t)(slot->advance.x >> 6),
        .bitmap_bytes = bitmap_bytes,
    };
    *out_glyph = glyph;
    size_t required_bytes = 0;
    esp_err_t result = copy_gray_bitmap(
        &slot->bitmap, bitmap, bitmap_capacity, &required_bytes);
    if ((result == ESP_OK || result == ESP_ERR_INVALID_SIZE) &&
        required_bytes != glyph.bitmap_bytes) {
        result = ESP_FAIL;
    }
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t ryz_font_rasterize_ascii(ryz_font_face_t face,
                                   uint32_t codepoint,
                                   uint16_t pixel_height,
                                   uint8_t *bitmap,
                                   size_t bitmap_capacity,
                                   ryz_font_glyph_t *out_glyph)
{
    if (codepoint < RYZ_FONT_ASCII_FIRST || codepoint > RYZ_FONT_ASCII_LAST)
        return ESP_ERR_INVALID_ARG;
    return ryz_font_rasterize(face, codepoint, pixel_height, bitmap,
                             bitmap_capacity, out_glyph);
}
