#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RYZ_UI_ID_MAX 24
#define RYZ_UI_TEXT_MAX 40
#define RYZ_UI_OBJECT_MAX 32

typedef enum {
    RYZ_UI_BOX = 0,
    RYZ_UI_LABEL,
    RYZ_UI_BUTTON,
    RYZ_UI_VIEWPORT,
    RYZ_UI_IMAGE,
} ryz_ui_object_kind_t;

typedef enum {
    RYZ_UI_FONT_BODY_16 = 0,
    RYZ_UI_FONT_TITLE_24,
    RYZ_UI_FONT_MONO_12,
    RYZ_UI_FONT_MONO_14,
    RYZ_UI_FONT_MONO_SEMIBOLD_12,
    RYZ_UI_FONT_MONO_SEMIBOLD_14,
    RYZ_UI_FONT_BUTTON_14,
    RYZ_UI_FONT_DISPLAY_20,
    RYZ_UI_FONT_MONO_MEDIUM_12,
    RYZ_UI_FONT_BODY_12,
    RYZ_UI_FONT_BODY_14,
    RYZ_UI_FONT_BUTTON_12,
    RYZ_UI_FONT_MEDIUM_14,
    RYZ_UI_FONT_BUTTON_16,
    RYZ_UI_FONT_MEDIUM_12,
    RYZ_UI_FONT_MONO_MEDIUM_13,
    RYZ_UI_FONT_COUNT,
} ryz_ui_font_t;

typedef enum {
    RYZ_UI_ALIGN_LEFT = 0,
    RYZ_UI_ALIGN_CENTER,
    RYZ_UI_ALIGN_RIGHT,
} ryz_ui_align_t;

typedef struct {
    char id[RYZ_UI_ID_MAX + 1];
    ryz_ui_object_kind_t kind;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    char text[RYZ_UI_TEXT_MAX + 1];
    uint16_t foreground;
    uint16_t background;
    uint16_t border;
    uint8_t border_width;
    uint8_t radius;
    ryz_ui_font_t font;
    ryz_ui_align_t align;
    bool visible;
    bool enabled;
    /* One-level, bounded clipping. Parent must be an earlier viewport.
     * Child coordinates are relative to its unscrolled content, max 1024px. */
    char parent[RYZ_UI_ID_MAX + 1];
    uint16_t content_height;
    uint16_t scroll_y;
    uint8_t line_height;
    char asset[RYZ_UI_ID_MAX + 1];
} ryz_ui_object_t;

typedef struct {
    char id[RYZ_UI_ID_MAX + 1];
    uint32_t generation;
    uint16_t background;
    uint8_t object_count;
    ryz_ui_object_t objects[RYZ_UI_OBJECT_MAX];
} ryz_ui_scene_t;

#ifdef __cplusplus
}
#endif
