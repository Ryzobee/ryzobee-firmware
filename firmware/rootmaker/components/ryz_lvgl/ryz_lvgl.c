#include "ryz_lvgl.h"
#include "ryz_lvgl_brand_font.h"
#include "ryz_v5_font.h"
#include "ryz_lvgl_system.h"
#include "ryz_lvgl_perf.h"
#include "ryz_lua_assets.h"

#include <stdatomic.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "ryz_lvgl_memory.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#if LV_COLOR_DEPTH != 16
#error "Ryzobee LVGL bridge requires native RGB565 rendering"
#endif

#if LV_TXT_ENC != LV_TXT_ENC_UTF8
#error "Ryzobee system labels require UTF-8 decoding (including Teko U+00B0)"
#endif

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_BUILTIN
#error "Ryzobee LVGL bridge requires the fixed-size built-in allocator"
#endif

#if !LV_DRAW_SW_SUPPORT_RGB565
#error "Ryzobee LVGL bridge requires software RGB565 rendering"
#endif

#if LV_DRAW_BUF_STRIDE_ALIGN != 1
#error "Ryzobee LVGL flush bridge requires tightly packed RGB565 rows"
#endif

#define RYZ_LVGL_DRAW_LINES 16
#define RYZ_LVGL_DRAW_BYTES \
    (RYZ_DISPLAY_WIDTH * RYZ_LVGL_DRAW_LINES * sizeof(uint16_t))

static const char *TAG = "ryz_lvgl";
typedef enum { LEASE_NONE, LEASE_LUA, LEASE_SYSTEM } lease_t;
typedef struct {
    /* One bounded copy owns every static label string for the tree lifetime.
     * Updates copy into these same arrays; caller buffers are never borrowed. */
    ryz_ui_scene_t scene;
    lv_obj_t *nodes[RYZ_UI_OBJECT_MAX];
    lv_obj_t *labels[RYZ_UI_OBJECT_MAX];
#if CONFIG_RYZ_LUA_FREETYPE
    ryz_lvgl_brand_font_t *fonts[RYZ_UI_FONT_COUNT];
#else
    const lv_font_t *fonts[RYZ_UI_FONT_COUNT];
#endif
    lv_font_t line_fonts[RYZ_UI_OBJECT_MAX];
    bool failed;
} lua_tree_t;
static lua_tree_t *s_lua_tree;
/* Also rejects synchronous callback re-entry, including void Lua release.
 * Never block the Owner waiting for another caller or for a callback to exit. */
static atomic_flag s_call_active = ATOMIC_FLAG_INIT;
static lease_t s_lease;
static lv_display_t *s_display;
static lv_obj_t *s_idle_screen;
static lv_obj_t *s_active_screen;
static lv_obj_t *s_system_candidate;
static uint8_t *s_draw_buffer;
#ifdef ESP_PLATFORM
static void *s_memory_pool;

void *ryz_lvgl_memory_pool(size_t bytes)
{
    configASSERT(s_memory_pool != NULL && bytes == LV_MEM_SIZE);
    return s_memory_pool;
}
#endif
static TaskHandle_t s_owner;
static esp_err_t s_flush_error;
static bool s_flush_cancelled;
static bool s_lvgl_initialized;
/* Borrowed only during synchronous owner-local refresh; never retained by
 * LVGL timers after mount/pump return. */
static bool (*s_cancelled)(void *);
static void *s_cancel_context;

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static lv_color_t color_from_rgb565(uint16_t color)
{
    uint8_t red = (uint8_t)((color >> 11) & 0x1f);
    uint8_t green = (uint8_t)((color >> 5) & 0x3f);
    uint8_t blue = (uint8_t)(color & 0x1f);
    red = (uint8_t)((red << 3) | (red >> 2));
    green = (uint8_t)((green << 2) | (green >> 4));
    blue = (uint8_t)((blue << 3) | (blue >> 2));
    return lv_color_make(red, green, blue);
}

static bool valid_identifier(const char *text)
{
    size_t length = text ? strnlen(text, RYZ_UI_ID_MAX + 1) : 0;
    if (!length || length > RYZ_UI_ID_MAX || text[0] < 'a' || text[0] > 'z') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        char value = text[i];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_')) return false;
    }
    return true;
}

static bool valid_text(const char *text, bool required)
{
    size_t length = text ? strnlen(text, RYZ_UI_TEXT_MAX + 1) : 0;
    if ((required && !length) || length > RYZ_UI_TEXT_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] != '\n' && ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e)) {
            return false;
        }
    }
    return true;
}

static bool valid_scene(const ryz_ui_scene_t *scene)
{
    if (!scene || !valid_identifier(scene->id) ||
        scene->object_count > RYZ_UI_OBJECT_MAX) return false;
    for (size_t i = 0; i < scene->object_count; ++i) {
        const ryz_ui_object_t *object = &scene->objects[i];
        if ((object->parent[0] && !valid_identifier(object->parent)) ||
            (object->asset[0] && !valid_identifier(object->asset))) return false;
        const ryz_ui_object_t *parent = NULL;
        if (object->parent[0]) {
            for (size_t j = 0; j < i; ++j)
                if (!strcmp(scene->objects[j].id, object->parent)) parent = &scene->objects[j];
            if (!parent || parent->kind != RYZ_UI_VIEWPORT ||
                object->x + object->width > parent->width ||
                object->y + object->height > parent->content_height) return false;
        }
        bool text = object->kind == RYZ_UI_LABEL || object->kind == RYZ_UI_BUTTON;
        if (!valid_identifier(object->id) ||
            (unsigned)object->kind > RYZ_UI_IMAGE ||
            object->x < 0 || object->y < 0 || !object->width || !object->height ||
            object->x > RYZ_DISPLAY_WIDTH - object->width ||
            (!parent && object->y > RYZ_DISPLAY_HEIGHT - object->height) ||
            object->border_width > 2 || object->radius > 32 ||
            (unsigned)object->font >= RYZ_UI_FONT_COUNT ||
            (unsigned)object->align > RYZ_UI_ALIGN_RIGHT ||
            !valid_text(object->text, text) || (!text && object->text[0]) ||
            (object->line_height && (object->line_height < 12 || object->line_height > 64))) return false;
        if (object->kind == RYZ_UI_VIEWPORT) {
            if (parent || object->content_height < object->height || object->content_height > 1024 ||
                object->scroll_y > object->content_height - object->height) return false;
        } else if (object->content_height || object->scroll_y) return false;
        if (object->kind == RYZ_UI_IMAGE) {
            bool board = !strcmp(object->asset,"rootmaker_a_face");
            if ((!board && strcmp(object->asset,"monitor_divider")) ||
                object->width != (board ? 88 : 232) || object->height != (board ? 88 : 2)) return false;
        } else if (object->asset[0]) return false;
        for (size_t prior = 0; prior < i; ++prior) {
            if (!strcmp(scene->objects[prior].id, object->id)) return false;
        }
    }
    return true;
}

static bool owner_matches(void)
{
    return s_owner == NULL || s_owner == xTaskGetCurrentTaskHandle();
}

static bool enter_call(void)
{
    return !atomic_flag_test_and_set_explicit(&s_call_active,
                                              memory_order_acquire);
}

static void leave_call(void)
{
    atomic_flag_clear_explicit(&s_call_active, memory_order_release);
}

bool ryz_lvgl_get_fps(uint16_t *fps)
{
    if (!fps) return false;
    *fps = 0;
    if (!enter_call()) return false;
    bool valid = false;
    if (s_display && s_lease != LEASE_NONE && s_owner && owner_matches())
        valid = ryz_lvgl_perf_read(s_display, fps);
    leave_call();
    return valid;
}

static esp_err_t font_status(const lua_tree_t *tree)
{
    if (!tree) return ESP_OK;
#if CONFIG_RYZ_LUA_FREETYPE
    for (unsigned i = 0; i < RYZ_UI_FONT_COUNT; ++i) {
        if (!tree->fonts[i]) continue;
        esp_err_t error = ryz_lvgl_brand_font_status(tree->fonts[i]);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
#else
    return ryz_v5_font_status();
#endif
}

/* All LVGL references must have been deleted before storage is destroyed. */
static void destroy_lua_storage(lua_tree_t *tree)
{
    if (!tree) return;
#if CONFIG_RYZ_LUA_FREETYPE
    for (unsigned i = 0; i < RYZ_UI_FONT_COUNT; ++i)
        ryz_lvgl_brand_font_destroy(tree->fonts[i]);
#endif
    heap_caps_free(tree);
}

static esp_err_t create_lua_storage(const ryz_ui_scene_t *scene, lua_tree_t **out)
{
    *out = NULL;
#if CONFIG_RYZ_LUA_FREETYPE
    if (!ryz_font_ready()) return ESP_ERR_INVALID_STATE;
#else
    ryz_v5_font_begin();
#endif
    lua_tree_t *tree = heap_caps_calloc(1, sizeof(*tree), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tree) return ESP_ERR_NO_MEM;
    tree->scene = *scene;
    static const struct { ryz_font_face_t face; uint16_t pixels; } specs[] = {
        {RYZ_FONT_BODY, 16}, {RYZ_FONT_BODY_SEMIBOLD, 24},
        {RYZ_FONT_MONO, 12}, {RYZ_FONT_MONO, 14},
        {RYZ_FONT_MONO_SEMIBOLD, 12}, {RYZ_FONT_MONO_SEMIBOLD, 14},
        {RYZ_FONT_BODY_SEMIBOLD, 14}, {RYZ_FONT_DISPLAY, 20},
        {RYZ_FONT_MONO_MEDIUM, 12},
        {RYZ_FONT_BODY, 12}, {RYZ_FONT_BODY, 14},
        {RYZ_FONT_BODY_SEMIBOLD, 12}, {RYZ_FONT_BODY_MEDIUM, 14},
        {RYZ_FONT_BODY_SEMIBOLD, 16}, {RYZ_FONT_BODY_MEDIUM, 12},
        {RYZ_FONT_MONO_MEDIUM, 13},
    };
    _Static_assert(sizeof(specs) / sizeof(specs[0]) == RYZ_UI_FONT_COUNT,
                   "Every Lua font needs a bounded cache specification");
    esp_err_t error = ESP_OK;
    /* Fixed Lua font names resolve to the same face/size in either build.
     * Static tables are shared with C UI, not a second copy or a fallback face. */
    for (unsigned i = 0; i < scene->object_count && error == ESP_OK; ++i) {
        const ryz_ui_object_t *object = &scene->objects[i];
        unsigned font = object->font;
        if ((object->kind != RYZ_UI_LABEL && object->kind != RYZ_UI_BUTTON) || tree->fonts[font]) continue;
#if CONFIG_RYZ_LUA_FREETYPE
        error = ryz_lvgl_brand_font_create(specs[font].face, specs[font].pixels,
                                           &tree->fonts[font]);
#else
        tree->fonts[font] = ryz_v5_font_get(specs[font].face, specs[font].pixels);
        if (!tree->fonts[font]) error = ESP_ERR_NOT_SUPPORTED;
#endif
    }
    if (error != ESP_OK) destroy_lua_storage(tree);
    else *out = tree;
    return error;
}

static bool can_claim(lease_t lease)
{
    return owner_matches() && (s_lease == LEASE_NONE || s_lease == lease);
}

static void claim(lease_t lease)
{
    s_owner = xTaskGetCurrentTaskHandle();
    s_lease = lease;
}

static void unclaim(void)
{
    s_owner = NULL;
    s_lease = LEASE_NONE;
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels)
{
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    bool is_last = lv_display_flush_is_last(display);
    if (s_flush_error == ESP_OK && s_cancelled &&
        s_cancelled(s_cancel_context)) {
        s_flush_error = ESP_ERR_TIMEOUT;
        s_flush_cancelled = true;
    }
    if (s_flush_error == ESP_OK) {
        s_flush_error = ryz_display_blit_rgb565_native(
            area->x1, area->y1, width, height, (const uint16_t *)pixels,
            (size_t)width * (size_t)height);
    }
    /* The board canvas owns a copy before the buffer is acknowledged. Only the
     * final area commits the accumulated dirty window. */
    if (s_flush_error == ESP_OK && is_last) {
        bool cancelled = false;
        s_flush_error = ryz_display_show_checked_status(
            s_cancelled, s_cancel_context, &cancelled);
        /* A drain/driver failure takes precedence over cancellation inside
         * show. Preserve its actual status rather than rechecking the callback. */
        s_flush_cancelled = cancelled;
    }
    lv_display_flush_ready(display);
}

/* Caller holds the public-call guard for all LVGL callbacks and flushes.
 * The cancellation context cannot escape this synchronous operation. */
static esp_err_t refresh(lv_obj_t *screen, bool (*cancelled)(void *),
                         void *context, bool *cancelled_out, const lua_tree_t *tree)
{
    s_flush_error = ESP_OK;
    s_flush_cancelled = false;
    s_cancelled = cancelled;
    s_cancel_context = context;
    if (screen) {
        lv_obj_invalidate(screen);
        lv_refr_now(s_display);
    } else {
        (void)lv_timer_handler();
    }
    s_cancelled = NULL;
    s_cancel_context = NULL;
    if (cancelled_out) *cancelled_out = s_flush_cancelled;
    return s_flush_error != ESP_OK ? s_flush_error : font_status(tree);
}

/* Consumes screen on success AND failure. Logical rollback is not a physical
 * repair: callers must keep input revoked until a later complete repaint. */
static esp_err_t present(lv_obj_t *screen, bool (*cancelled)(void *),
                         void *context, bool *cancelled_out, const lua_tree_t *tree)
{
    if (cancelled && cancelled(context)) {
        if (cancelled_out) *cancelled_out = true;
        lv_obj_delete(screen);
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *previous = s_active_screen;
    lv_screen_load(screen);
    esp_err_t error = refresh(screen, cancelled, context, cancelled_out, tree);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "scene refresh failed: %s", esp_err_to_name(error));
        lv_screen_load(previous ? previous : s_idle_screen);
        lv_obj_delete(screen);
        return error;
    }
    s_active_screen = screen;
    if (previous) lv_obj_delete(previous);
    return ESP_OK;
}

static void release_roots(void)
{
    lv_screen_load(s_idle_screen);
    if (s_system_candidate) {
        lv_obj_delete(s_system_candidate);
        s_system_candidate = NULL;
    }
    if (s_active_screen) {
        lv_obj_delete(s_active_screen);
        s_active_screen = NULL;
    }
    destroy_lua_storage(s_lua_tree);
    s_lua_tree = NULL;
}

static esp_err_t initialize(void)
{
    if (s_display) return ESP_OK;
    s_draw_buffer = heap_caps_malloc(
        RYZ_LVGL_DRAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_draw_buffer) return ESP_ERR_NO_MEM;
    if (!s_lvgl_initialized) {
#ifdef ESP_PLATFORM
        /* Allocate before calling LVGL, whose builtin pool hook cannot report
         * initialization failure. Like its former .bss pool this lives for
         * the boot, across scenes and failed display-creation retries. */
        if (!s_memory_pool) {
            s_memory_pool = heap_caps_malloc(
                LV_MEM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_memory_pool) {
                heap_caps_free(s_draw_buffer);
                s_draw_buffer = NULL;
                return ESP_ERR_NO_MEM;
            }
        }
#endif
        lv_init();
        lv_tick_set_cb(tick_ms);
        s_lvgl_initialized = true;
    }
    s_display = lv_display_create(RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
    if (!s_display) goto no_memory;
    if (ryz_lvgl_perf_init(s_display) != ESP_OK) goto no_memory;
    lv_display_set_default(s_display);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL,
                           RYZ_LVGL_DRAW_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush_display);

    s_idle_screen = lv_obj_create(NULL);
    if (!s_idle_screen) goto no_memory;
    lv_obj_remove_style_all(s_idle_screen);
    lv_obj_set_size(s_idle_screen, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(s_idle_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_idle_screen, LV_OPA_COVER, 0);
    lv_screen_load(s_idle_screen);
    return ESP_OK;

no_memory:
    if (s_display) lv_display_delete(s_display);
    s_display = NULL;
    s_idle_screen = NULL;
    heap_caps_free(s_draw_buffer);
    s_draw_buffer = NULL;
    return ESP_ERR_NO_MEM;
}

static const lv_font_t *font_for(const lua_tree_t *tree, const ryz_ui_object_t *object)
{
    return &tree->line_fonts[object - tree->scene.objects];
}

static lv_text_align_t text_align_for(ryz_ui_align_t align)
{
    if (align == RYZ_UI_ALIGN_CENTER) return LV_TEXT_ALIGN_CENTER;
    if (align == RYZ_UI_ALIGN_RIGHT) return LV_TEXT_ALIGN_RIGHT;
    return LV_TEXT_ALIGN_LEFT;
}

static void style_box(lv_obj_t *node, const ryz_ui_object_t *object)
{
    lv_obj_remove_style_all(node);
    lv_obj_set_pos(node, object->x, object->y);
    lv_obj_set_size(node, object->width, object->height);
    lv_obj_set_style_bg_color(node, color_from_rgb565(object->background), 0);
    lv_obj_set_style_bg_opa(node, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(node, color_from_rgb565(object->border), 0);
    lv_obj_set_style_border_width(node, object->border_width, 0);
    lv_obj_set_style_radius(node, object->radius, 0);
    lv_obj_remove_flag(node, LV_OBJ_FLAG_SCROLLABLE);
    if (!object->visible) lv_obj_add_flag(node, LV_OBJ_FLAG_HIDDEN);
    if (!object->enabled) lv_obj_set_style_bg_opa(node, LV_OPA_40, 0);
}

static void style_label(lv_obj_t *label, const ryz_ui_object_t *object,
                        bool child, const lua_tree_t *tree)
{
    lv_obj_remove_style_all(label);
    if (child) {
        int width = object->width > 8 ? object->width - 8 : object->width;
        lv_obj_set_width(label, width);
    } else {
        lv_obj_set_pos(label, object->x, object->y);
        lv_obj_set_size(label, object->width, object->height);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text_static(label, object->text);
    lv_obj_set_style_text_color(label, color_from_rgb565(object->foreground), 0);
    lv_obj_set_style_text_font(label, font_for(tree, object), 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_obj_set_style_text_align(label, text_align_for(object->align), 0);
    if (!object->visible) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_opa(label, object->enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    if (child) lv_obj_center(label);
}

static lv_obj_t *build_screen(lua_tree_t *tree)
{
    const ryz_ui_scene_t *scene = &tree->scene;
    lv_obj_t *screen = lv_obj_create(NULL);
    if (!screen) return NULL;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(screen, color_from_rgb565(scene->background), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < scene->object_count; ++i) {
        const ryz_ui_object_t *object = &scene->objects[i];
        lv_obj_t *parent = screen;
        if (object->parent[0]) for (size_t j = 0; j < i; ++j)
            if (!strcmp(scene->objects[j].id,object->parent)) parent = tree->nodes[j];
        if (object->kind == RYZ_UI_LABEL || object->kind == RYZ_UI_BUTTON) {
#if CONFIG_RYZ_LUA_FREETYPE
            tree->line_fonts[i] = *ryz_lvgl_brand_font_get(tree->fonts[object->font]);
#else
            tree->line_fonts[i] = *tree->fonts[object->font];
#endif
            if (object->line_height) {
                int delta = object->line_height - tree->line_fonts[i].line_height;
                tree->line_fonts[i].base_line += delta - delta / 2;
                tree->line_fonts[i].line_height = object->line_height;
            }
        }
        if (object->kind == RYZ_UI_IMAGE) {
            lv_obj_t *image = lv_image_create(parent);
            if (!image) { lv_obj_delete(screen); return NULL; }
            tree->nodes[i] = image;
            lv_image_set_src(image, !strcmp(object->asset,"rootmaker_a_face") ?
                &ryz_lua_rootmaker_a_face : &ryz_lua_monitor_divider);
            lv_obj_set_pos(image,object->x,object->y);
            if (!object->visible) lv_obj_add_flag(image,LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (object->kind == RYZ_UI_LABEL) {
            lv_obj_t *label = lv_label_create(parent);
            if (!label) { lv_obj_delete(screen); return NULL; }
            tree->nodes[i] = tree->labels[i] = label;
            style_label(label, object, false, tree);
            continue;
        }
        lv_obj_t *box = lv_obj_create(parent);
        if (!box) { lv_obj_delete(screen); return NULL; }
        tree->nodes[i] = box;
        style_box(box, object);
        if (object->kind == RYZ_UI_BUTTON) {
            lv_obj_t *label = lv_label_create(box);
            if (!label) { lv_obj_delete(screen); return NULL; }
            tree->labels[i] = label;
            style_label(label, object, true, tree);
        }
    }
    for (size_t i = 0; i < scene->object_count; ++i) {
        const ryz_ui_object_t *o = &scene->objects[i];
        if (o->parent[0]) for (size_t j = 0; j < i; ++j)
            if (!strcmp(scene->objects[j].id,o->parent))
                lv_obj_set_y(tree->nodes[i],o->y - scene->objects[j].scroll_y);
    }
    return screen;
}

esp_err_t ryz_lvgl_mount(const ryz_ui_scene_t *scene)
{
    return ryz_lvgl_mount_checked(scene, NULL, NULL);
}

esp_err_t ryz_lvgl_mount_checked(const ryz_ui_scene_t *scene,
                                 bool (*cancelled)(void *), void *context)
{
    if (!valid_scene(scene)) return ESP_ERR_INVALID_ARG;
    if (!enter_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!can_claim(LEASE_LUA)) goto done;
    if (cancelled && cancelled(context)) {
        error = ESP_ERR_TIMEOUT;
        goto done;
    }
    bool newly_claimed = s_lease == LEASE_NONE;
    if (newly_claimed) claim(LEASE_LUA);
    error = initialize();
    if (error == ESP_OK) {
        lua_tree_t *tree = NULL;
        error = create_lua_storage(scene, &tree);
        if (error == ESP_OK) {
            lv_obj_t *screen = build_screen(tree);
            error = screen ? present(screen, cancelled, context, NULL, tree) : ESP_ERR_NO_MEM;
            if (error == ESP_OK) {
                destroy_lua_storage(s_lua_tree);
                s_lua_tree = tree;
            } else destroy_lua_storage(tree);
        }
    }
    if (error != ESP_OK && newly_claimed) unclaim();
done:
    leave_call();
    return error;
}

esp_err_t ryz_lvgl_pump(void)
{
    return ryz_lvgl_pump_checked(NULL, NULL);
}

esp_err_t ryz_lvgl_update(const ryz_ui_scene_t *scene)
{
    return ryz_lvgl_update_checked(scene, NULL, NULL);
}

static bool same_layout(const ryz_ui_scene_t *left, const ryz_ui_scene_t *right)
{
    if (strcmp(left->id, right->id) || left->generation != right->generation ||
        left->object_count != right->object_count) return false;
    for (size_t i = 0; i < left->object_count; ++i) {
        const ryz_ui_object_t *a = &left->objects[i], *b = &right->objects[i];
        if (strcmp(a->id, b->id) || a->kind != b->kind || a->x != b->x ||
            (a->kind != RYZ_UI_BOX && (a->y != b->y || a->height != b->height || a->width != b->width)) ||
            a->font != b->font ||
            a->align != b->align || a->radius != b->radius || a->border_width != b->border_width ||
            strcmp(a->parent,b->parent) || strcmp(a->asset,b->asset) ||
            a->content_height != b->content_height || a->line_height != b->line_height)
            return false;
    }
    return true;
}

static void set_visible(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void update_tree(lua_tree_t *tree, const ryz_ui_scene_t *scene)
{
    if(tree->scene.background!=scene->background)
        lv_obj_set_style_bg_color(s_active_screen, color_from_rgb565(scene->background), 0);
    for (size_t i = 0; i < scene->object_count; ++i) {
        const ryz_ui_object_t *object = &scene->objects[i];
        ryz_ui_object_t *old = &tree->scene.objects[i];
        bool text_changed=strcmp(old->text,object->text)!=0;
        lv_obj_t *node = tree->nodes[i], *label = tree->labels[i];
        if (object->kind == RYZ_UI_BOX) {
            if(!object->parent[0] && old->y!=object->y) lv_obj_set_y(node,object->y);
            if(old->height!=object->height) lv_obj_set_height(node,object->height);
            if(old->width!=object->width) lv_obj_set_width(node,object->width);
        }
        if(old->visible!=object->visible) set_visible(node, object->visible);
        if (object->kind != RYZ_UI_LABEL && object->kind != RYZ_UI_IMAGE) {
            if(old->background!=object->background)
                lv_obj_set_style_bg_color(node, color_from_rgb565(object->background), 0);
            if(old->enabled!=object->enabled)
                lv_obj_set_style_bg_opa(node, object->enabled ? LV_OPA_COVER : LV_OPA_40, 0);
            if(old->border!=object->border)
                lv_obj_set_style_border_color(node, color_from_rgb565(object->border), 0);
        }
        if (object->parent[0]) for (size_t j = 0; j < i; ++j)
            if (!strcmp(scene->objects[j].id,object->parent))
                lv_obj_set_y(node,object->y - scene->objects[j].scroll_y);
        if (label) {
            if(label!=node && old->visible!=object->visible) set_visible(label, object->visible);
            if(old->foreground!=object->foreground)
                lv_obj_set_style_text_color(label, color_from_rgb565(object->foreground), 0);
            if(old->enabled!=object->enabled)
                lv_obj_set_style_text_opa(label, object->enabled ? LV_OPA_COVER : LV_OPA_40, 0);
        }
        /* Copy only after comparisons. The label keeps pointing into this
         * tree-owned array, never the caller's candidate/stack. */
        *old=*object;
        if(label && text_changed) {
            lv_label_set_text_static(label,old->text);
            if(object->kind==RYZ_UI_BUTTON) lv_obj_center(label);
        }
    }
    tree->scene.background=scene->background;
}

esp_err_t ryz_lvgl_update_checked(const ryz_ui_scene_t *scene,
                                  bool (*cancelled)(void *), void *context)
{
    if (!valid_scene(scene)) return ESP_ERR_INVALID_ARG;
    if (!enter_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!s_owner || !owner_matches() || s_lease != LEASE_LUA || !s_active_screen ||
        !s_lua_tree || s_lua_tree->failed || !same_layout(&s_lua_tree->scene, scene)) goto done;
    if (cancelled && cancelled(context)) { error = ESP_ERR_TIMEOUT; goto done; }
    update_tree(s_lua_tree, scene);
    /* Force the refresh timer, not the root's dirty area. Still synchronous:
     * changed labels/layout reach the checked BSP flush before acknowledging. */
    lv_timer_ready(lv_display_get_refr_timer(s_display));
    error = refresh(NULL, cancelled, context, NULL, s_lua_tree);
    if (error != ESP_OK) s_lua_tree->failed = true;
done:
    leave_call();
    return error;
}

esp_err_t ryz_lvgl_pump_checked(bool (*cancelled)(void *), void *context)
{
    if (!enter_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_display && s_owner && owner_matches() && s_lease == LEASE_LUA &&
        s_lua_tree && !s_lua_tree->failed) {
        error = cancelled && cancelled(context) ? ESP_ERR_TIMEOUT :
                refresh(NULL, cancelled, context, NULL, s_lua_tree);
    }
    leave_call();
    return error;
}

void ryz_lvgl_release(void)
{
    if (!enter_call()) return;
    if (s_display && s_owner && owner_matches() && s_lease == LEASE_LUA) {
        release_roots();
        unclaim();
    }
    leave_call();
}

esp_err_t ryz_lvgl_system_create(lv_obj_t **out_root)
{
    if (!out_root) return ESP_ERR_INVALID_ARG;
    *out_root = NULL;
    if (!enter_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (!can_claim(LEASE_SYSTEM) || s_system_candidate) goto done;
    bool newly_claimed = s_lease == LEASE_NONE;
    if (newly_claimed) claim(LEASE_SYSTEM);
    error = initialize();
    if (error == ESP_OK) {
        s_system_candidate = lv_obj_create(NULL);
        if (!s_system_candidate) {
            error = ESP_ERR_NO_MEM;
        } else {
            lv_obj_remove_style_all(s_system_candidate);
            lv_obj_set_size(s_system_candidate, RYZ_DISPLAY_WIDTH, RYZ_DISPLAY_HEIGHT);
            lv_obj_set_style_bg_color(s_system_candidate, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(s_system_candidate, LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_system_candidate, LV_OBJ_FLAG_SCROLLABLE);
            *out_root = s_system_candidate;
        }
    }
    if (error != ESP_OK && newly_claimed) unclaim();
done:
    leave_call();
    return error;
}

esp_err_t ryz_lvgl_system_commit(lv_obj_t *candidate,
                                bool (*cancelled)(void *), void *context,
                                bool *cancelled_out)
{
    if (cancelled_out) *cancelled_out = false;
    if (!candidate) return ESP_ERR_INVALID_ARG;
    if (!enter_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_lease != LEASE_SYSTEM || !s_owner || !owner_matches()) goto done;
    if (candidate != s_system_candidate) {
        error = ESP_ERR_INVALID_ARG;
        goto done;
    }
    s_system_candidate = NULL;
    error = present(candidate, cancelled, context, cancelled_out, NULL);
done:
    leave_call();
    return error;
}

esp_err_t ryz_lvgl_system_pump(bool (*cancelled)(void *), void *context,
                              bool *cancelled_out)
{
    if (cancelled_out) *cancelled_out = false;
    if (!enter_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_lease == LEASE_SYSTEM && s_owner && owner_matches() && s_active_screen) {
        if (cancelled && cancelled(context)) {
            if (cancelled_out) *cancelled_out = true;
            error = ESP_ERR_TIMEOUT;
        } else {
            error = refresh(NULL, cancelled, context, cancelled_out, NULL);
        }
    }
    leave_call();
    return error;
}

esp_err_t ryz_lvgl_system_release(void)
{
    if (!enter_call()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (s_lease == LEASE_NONE) {
        error = ESP_OK;
    } else if (s_lease == LEASE_SYSTEM && s_owner && owner_matches()) {
        release_roots();
        /* The synchronous flush and all deletion callbacks have returned.
         * Retain both the lease and public-call guard until every scratch
         * byte is erased: the next Lua/system lease must not inherit pixels
         * from a sensitive system page. Volatile stores may not be elided.
         * The BSP canvas/DMA/panel privacy barrier is separately owned. */
        volatile uint8_t *bytes = s_draw_buffer;
        if (bytes) {
            for (size_t i = 0; i < RYZ_LVGL_DRAW_BYTES; ++i) bytes[i] = 0;
        }
        unclaim();
        error = ESP_OK;
    }
    leave_call();
    return error;
}
