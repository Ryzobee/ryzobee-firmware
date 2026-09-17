#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ryz_display_host.h"
#include "ryz_lvgl.h"
#include "ryz_lvgl_system.h"
#include "ryz_pixels_host.h"
#include "ryz_font.h"
#include "ryz_v5_font.h"

#ifdef NDEBUG
#error "Lifecycle pixel checks require active assertions"
#endif

_Static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 5 &&
               LVGL_VERSION_PATCH == 0, "Expected locked LVGL 9.5.0");
_Static_assert(LV_COLOR_DEPTH == 16 && LV_MEM_SIZE == 64 * 1024 &&
               LV_DRAW_BUF_STRIDE_ALIGN == 1 && LV_USE_OS == LV_OS_NONE &&
               LV_DRAW_SW_DRAW_UNIT_CNT == 1, "Target pixel/owner configuration changed");

static unsigned cases;
static void passed(const char *name) { ++cases; printf("PASS %s\n", name); }

static ryz_ui_scene_t lua_scene(void)
{
    ryz_ui_scene_t scene = {.id = "legacy_scene", .generation = 1,
                            .background = 0xf800, .object_count = 3};
    scene.objects[0] = (ryz_ui_object_t){
        .id = "box", .kind = RYZ_UI_BOX, .x = 10, .y = 10,
        .width = 24, .height = 24, .background = 0x07e0,
        .border = 0xffff, .border_width = 2, .visible = true, .enabled = true,
    };
    scene.objects[1] = (ryz_ui_object_t){
        .id = "title", .kind = RYZ_UI_LABEL, .x = 45, .y = 10,
        .width = 180, .height = 40, .text = "RYZOBEE",
        .foreground = 0xffff, .font = RYZ_UI_FONT_BODY_16,
        .visible = true, .enabled = true,
    };
    scene.objects[2] = (ryz_ui_object_t){
        .id = "button", .kind = RYZ_UI_BUTTON, .x = 10, .y = 80,
        .width = 100, .height = 40, .text = "GO", .background = 0x001f,
        .foreground = 0xffff, .border = 0xffff, .border_width = 1,
        .font = RYZ_UI_FONT_TITLE_24, .align = RYZ_UI_ALIGN_CENTER,
        .visible = true, .enabled = true,
    };
    return scene;
}

static void expect_pixel(unsigned x, unsigned y, uint16_t color)
{
    const uint16_t actual = ryz_host_display_pixel(x, y);
    if (actual != color) fprintf(stderr, "pixel %u,%u=%04x expected=%04x\n",
                                 x, y, actual, color);
    assert(actual == color);
}

static size_t count_pixels(unsigned x, unsigned y, unsigned width,
                            unsigned height, uint16_t not_color)
{
    size_t count = 0;
    for (unsigned row = y; row < y + height; ++row)
        for (unsigned column = x; column < x + width; ++column)
            if (ryz_host_display_pixel(column, row) != not_color) ++count;
    return count;
}

static void deleted(lv_event_t *event)
{
    unsigned *count = lv_event_get_user_data(event);
    ++*count;
}

static lv_obj_t *system_root(uint32_t rgb, unsigned *delete_count)
{
    lv_obj_t *root = NULL;
    assert(ryz_lvgl_system_create(&root) == ESP_OK && root);
    lv_obj_set_style_bg_color(root, lv_color_hex(rgb), 0);
    if (delete_count) assert(lv_obj_add_event_cb(root, deleted, LV_EVENT_DELETE, delete_count));
    return root;
}

static lv_obj_t *rectangle(lv_obj_t *parent, int x, int y, int width,
                            int height, uint32_t rgb)
{
    lv_obj_t *object = lv_obj_create(parent);
    assert(object);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static void commit(lv_obj_t *root)
{
    bool cancelled = true;
    assert(ryz_lvgl_system_commit(root, NULL, NULL, &cancelled) == ESP_OK);
    assert(!cancelled);
    assert(lv_screen_active() == root);
}

static void repaint(lv_obj_t *root)
{
    lv_obj_invalidate(root);
    ryz_host_display_advance_ms(100);
    const size_t shows = ryz_host_display_shows();
    bool cancelled = true;
    assert(ryz_lvgl_system_pump(NULL, NULL, &cancelled) == ESP_OK && !cancelled);
    assert(ryz_host_display_shows() > shows);
}

static bool cancel_now(void *opaque)
{
    unsigned *calls = opaque;
    ++*calls;
    return true;
}

static bool never_cancel(void *opaque)
{
    unsigned *calls = opaque;
    ++*calls;
    return false;
}

typedef struct { size_t after_rows; unsigned calls; } transfer_cancel_t;
static bool cancel_after_transfer(void *opaque)
{
    transfer_cancel_t *condition = opaque;
    ++condition->calls;
    return ryz_host_display_rows() >= condition->after_rows;
}

static void bootstrap(void)
{
    bool cancelled = true;
    assert(ryz_lvgl_system_release() == ESP_OK);
    assert(ryz_lvgl_system_create(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_lvgl_system_pump(NULL, NULL, &cancelled) == ESP_ERR_INVALID_STATE);
    assert(!cancelled);
    assert(ryz_lvgl_pump() == ESP_ERR_INVALID_STATE);
    ryz_host_heap_fail_after(0);
    lv_obj_t *root = (void *)(uintptr_t)1;
    assert(ryz_lvgl_system_create(&root) == ESP_ERR_NO_MEM && !root);
    assert(!ryz_host_heap_bytes() && !ryz_host_heap_blocks());
    const ryz_ui_scene_t scene = lua_scene();
    assert(ryz_lvgl_mount(&scene) == ESP_ERR_NO_MEM);
    assert(ryz_lvgl_system_release() == ESP_OK);
    ryz_host_heap_allow();
    /* Only the external draw allocation is fault-injected. This does not
     * claim that arbitrary LVGL internal pool exhaustion is recoverable. */
    passed("draw-buffer allocation failure releases first lease / retry");
}

typedef struct { lv_obj_t *candidate; const ryz_ui_scene_t *scene; } foreign_t;
static void *foreign_calls(void *opaque)
{
    const foreign_t *context = opaque;
    lv_obj_t *root = (void *)(uintptr_t)1;
    assert(ryz_lvgl_system_create(&root) == ESP_ERR_INVALID_STATE && !root);
    bool cancelled = true;
    assert(ryz_lvgl_system_commit(context->candidate, NULL, NULL, &cancelled) ==
           ESP_ERR_INVALID_STATE && !cancelled);
    cancelled = true;
    assert(ryz_lvgl_system_pump(NULL, NULL, &cancelled) == ESP_ERR_INVALID_STATE && !cancelled);
    assert(ryz_lvgl_system_release() == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_mount(context->scene) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_update(context->scene) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_pump() == ESP_ERR_INVALID_STATE);
    ryz_lvgl_release();
    return NULL;
}

static void check_foreign(lv_obj_t *root, const ryz_ui_scene_t *scene)
{
    const size_t shows = ryz_host_display_shows();
    const size_t bytes = ryz_host_heap_bytes();
    foreign_t context = {.candidate = root, .scene = scene};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, foreign_calls, &context) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(ryz_host_display_shows() == shows && ryz_host_heap_bytes() == bytes);
}

static void system_pixels_and_ownership(void)
{
    unsigned a_deleted = 0, b_deleted = 0, c_deleted = 0;
    const ryz_ui_scene_t scene = lua_scene();
    const size_t shows = ryz_host_display_shows();
    lv_obj_t *a = system_root(0xff0000, &a_deleted);
    assert(ryz_host_heap_bytes() == 240U * 16U * sizeof(uint16_t));
    assert(ryz_host_heap_blocks() == 1);
    assert(ryz_host_display_shows() == shows);
    assert(lv_obj_get_parent(a) == NULL && !lv_obj_has_flag(a, LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t *duplicate = (void *)(uintptr_t)1;
    assert(ryz_lvgl_system_create(&duplicate) == ESP_ERR_INVALID_STATE && !duplicate);
    bool cancelled = true;
    assert(ryz_lvgl_system_pump(NULL, NULL, &cancelled) == ESP_ERR_INVALID_STATE && !cancelled);
    assert(ryz_lvgl_mount(&scene) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_update(&scene) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_pump() == ESP_ERR_INVALID_STATE);
    ryz_lvgl_release();
    check_foreign(a, &scene);
    lv_obj_t *green = rectangle(a, 10, 10, 30, 20, 0x00ff00);
    for (int i = 0; i < 37; ++i) rectangle(a, 2 + i * 6, 200, 4, 4, 0xffffff);
    lv_obj_t *label = lv_label_create(a);
    assert(label);
    lv_obj_set_pos(label, 4, 50);
    lv_obj_set_size(label, 230, 80);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    const char *long_text = "SYSTEM PAGE HAS MORE THAN FORTY CHARACTERS AND THIRTY TWO CHILDREN";
    lv_label_set_text(label, long_text);
    assert(lv_obj_get_child_count(a) > RYZ_UI_OBJECT_MAX && strlen(long_text) > RYZ_UI_TEXT_MAX);
    cancelled = true;
    assert(ryz_lvgl_system_commit(NULL, NULL, NULL, &cancelled) == ESP_ERR_INVALID_ARG && !cancelled);
    assert(ryz_lvgl_system_commit(green, NULL, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(a_deleted == 0);
    commit(a);
    assert(lv_obj_get_width(a) == 240 && lv_obj_get_height(a) == 240);
    expect_pixel(0, 0, 0xf800);
    expect_pixel(20, 20, 0x07e0);
    expect_pixel(3, 201, 0xffff);
    assert(count_pixels(4, 50, 230, 80, 0xf800) > 100);

    lv_obj_set_style_bg_color(green, lv_color_hex(0x0000ff), 0);
    repaint(a);
    expect_pixel(20, 20, 0x001f);
    lv_obj_t *b = system_root(0x00ff00, &b_deleted);
    repaint(a); /* A pending candidate must not be accidentally loaded. */
    assert(lv_screen_active() == a && a_deleted == 0 && b_deleted == 0);
    expect_pixel(0, 0, 0xf800);
    check_foreign(b, &scene);
    commit(b);
    assert(a_deleted == 1 && b_deleted == 0);
    expect_pixel(0, 0, 0x07e0);
    (void)system_root(0x0000ff, &c_deleted);
    assert(ryz_lvgl_system_release() == ESP_OK);
    assert(b_deleted == 1 && c_deleted == 1);
    expect_pixel(0, 0, 0x07e0); /* Release does not present the idle screen. */
    assert(ryz_lvgl_system_release() == ESP_OK);
    assert(ryz_lvgl_system_pump(NULL, NULL, NULL) == ESP_ERR_INVALID_STATE);
    passed("system candidate/active roots, >32 children, long text, pixels and owner exclusion");
}

static void legacy_wrapper(void)
{
    ryz_ui_scene_t scene = lua_scene();
    assert(ryz_lvgl_mount(&scene) == ESP_OK);
    expect_pixel(0, 0, 0xf800);
    expect_pixel(10, 10, 0xffff);
    expect_pixel(20, 20, 0x07e0);
    expect_pixel(12, 82, 0x001f);
    assert(count_pixels(45, 10, 180, 40, 0xf800) > 50);
    check_foreign(lv_screen_active(), &scene);
    lv_obj_t *candidate = (void *)(uintptr_t)1;
    assert(ryz_lvgl_system_create(&candidate) == ESP_ERR_INVALID_STATE && !candidate);
    assert(ryz_lvgl_system_release() == ESP_ERR_INVALID_STATE);
    ryz_ui_scene_t invalid = scene;
    invalid.object_count = RYZ_UI_OBJECT_MAX + 1;
    assert(ryz_lvgl_mount(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = scene;
    strcpy(invalid.objects[1].id, invalid.objects[0].id);
    assert(ryz_lvgl_mount(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = scene;
    invalid.objects[1].text[0] = (char)0x80;
    assert(ryz_lvgl_mount(&invalid) == ESP_ERR_INVALID_ARG);
    unsigned cancellation_calls = 0;
    assert(ryz_lvgl_mount_checked(&scene, cancel_now, &cancellation_calls) == ESP_ERR_TIMEOUT);
    assert(cancellation_calls > 0);
    expect_pixel(0, 0, 0xf800);
    scene.background = 0x001f;
    ryz_host_display_fail_show(ESP_ERR_TIMEOUT, 16);
    assert(ryz_lvgl_mount(&scene) == ESP_ERR_TIMEOUT);
    expect_pixel(0, 0, 0x001f);
    expect_pixel(0, 100, 0xf800);
    ryz_host_display_advance_ms(100);
    assert(ryz_lvgl_pump() == ESP_OK);
    expect_pixel(0, 0, 0xf800);
    assert(ryz_lvgl_mount(&scene) == ESP_OK);
    expect_pixel(0, 0, 0x001f);
    ryz_lvgl_release();
    assert(ryz_lvgl_pump() == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_system_release() == ESP_OK);
    passed("legacy Lua boxes/labels/buttons, schema validation, rollback and lease transfer");
}

static void failures_and_cancellation(void)
{
    unsigned rejected = 0;
    lv_obj_t *candidate = system_root(0x0000ff, &rejected);
    unsigned cancellation_calls = 0;
    bool cancelled = false;
    assert(ryz_lvgl_system_commit(candidate, cancel_now, &cancellation_calls, &cancelled) ==
           ESP_ERR_TIMEOUT && cancelled && rejected == 1);
    const ryz_ui_scene_t scene = lua_scene();
    assert(ryz_lvgl_mount(&scene) == ESP_ERR_INVALID_STATE); /* Failed commit retains lease. */
    assert(ryz_lvgl_system_pump(NULL, NULL, NULL) == ESP_ERR_INVALID_STATE);
    unsigned retained = 0;
    lv_obj_t *active = system_root(0xff0000, &retained);
    commit(active);
    candidate = system_root(0x0000ff, &rejected);
    ryz_host_display_fail_blit(ESP_FAIL);
    cancelled = true;
    assert(ryz_lvgl_system_commit(candidate, NULL, NULL, &cancelled) == ESP_FAIL && !cancelled);
    assert(rejected == 2 && retained == 0 && lv_screen_active() == active);
    repaint(active);
    expect_pixel(0, 0, 0xf800);

    candidate = system_root(0x0000ff, &rejected);
    ryz_host_display_fail_show(ESP_ERR_TIMEOUT, 16);
    cancellation_calls = 0;
    cancelled = true;
    assert(ryz_lvgl_system_commit(candidate, never_cancel, &cancellation_calls, &cancelled) ==
           ESP_ERR_TIMEOUT && !cancelled && cancellation_calls > 0);
    assert(rejected == 3 && retained == 0 && lv_screen_active() == active);
    expect_pixel(0, 0, 0x001f);
    expect_pixel(0, 100, 0xf800); /* Logical rollback does not claim pixel repair. */
    const unsigned previous_calls = cancellation_calls;
    repaint(active);
    assert(cancellation_calls == previous_calls); /* Callback was not retained. */
    expect_pixel(0, 0, 0xf800);

    candidate = system_root(0x0000ff, &rejected);
    transfer_cancel_t condition = {.after_rows = ryz_host_display_rows() + 16};
    cancelled = false;
    assert(ryz_lvgl_system_commit(candidate, cancel_after_transfer, &condition, &cancelled) ==
           ESP_ERR_TIMEOUT && cancelled && condition.calls > 0);
    assert(rejected == 4 && retained == 0);
    expect_pixel(0, 0, 0x001f);
    expect_pixel(0, 100, 0xf800);
    repaint(active);

    candidate = system_root(0x0000ff, &rejected);
    condition = (transfer_cancel_t){.after_rows = ryz_host_display_rows() + 16};
    ryz_host_display_fail_cancel_drain(ESP_ERR_TIMEOUT);
    cancelled = true;
    assert(ryz_lvgl_system_commit(candidate, cancel_after_transfer, &condition, &cancelled) ==
           ESP_ERR_TIMEOUT && !cancelled);
    assert(rejected == 5 && retained == 0);
    repaint(active);
    cancellation_calls = 0;
    cancelled = false;
    assert(ryz_lvgl_system_pump(cancel_now, &cancellation_calls, &cancelled) ==
           ESP_ERR_TIMEOUT && cancelled);
    assert(retained == 0 && lv_screen_active() == active);
    lv_obj_invalidate(active);
    ryz_host_display_advance_ms(100);
    ryz_host_display_fail_show(ESP_ERR_TIMEOUT, 0);
    cancelled = true;
    assert(ryz_lvgl_system_pump(NULL, NULL, &cancelled) == ESP_ERR_TIMEOUT && !cancelled);
    repaint(active);
    assert(ryz_lvgl_system_release() == ESP_OK && retained == 1);
    passed("candidate consumption, logical rollback, partial frames, cancellation vs driver/drain timeout");
}

typedef struct { lv_obj_t *candidate; unsigned calls; } reentry_t;
static void attempt_reentry(reentry_t *context)
{
    ++context->calls;
    lv_obj_t *root = (void *)(uintptr_t)1;
    assert(ryz_lvgl_system_create(&root) == ESP_ERR_INVALID_STATE && !root);
    bool cancelled = true;
    assert(ryz_lvgl_system_commit(context->candidate, NULL, NULL, &cancelled) ==
           ESP_ERR_INVALID_STATE && !cancelled);
    assert(ryz_lvgl_system_pump(NULL, NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_system_release() == ESP_ERR_INVALID_STATE);
    const ryz_ui_scene_t scene = lua_scene();
    assert(ryz_lvgl_mount(&scene) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_pump() == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_update(&scene) == ESP_ERR_INVALID_STATE);
    ryz_lvgl_release();
}
static bool cancellation_reentry(void *opaque)
{
    reentry_t *context = opaque;
    if (!context->calls) attempt_reentry(context);
    return false;
}
static void event_reentry(lv_event_t *event)
{
    attempt_reentry(lv_event_get_user_data(event));
}

static void callback_guards(void)
{
    /* Deliberate misuse verifies the defensive guard; callbacks remain
     * contractually forbidden from reentering the bridge or mutating roots. */
    unsigned deleted_count = 0;
    lv_obj_t *root = system_root(0xff0000, &deleted_count);
    reentry_t cancellation = {.candidate = root};
    bool cancelled = true;
    assert(ryz_lvgl_system_commit(root, cancellation_reentry, &cancellation, &cancelled) == ESP_OK);
    assert(!cancelled && cancellation.calls == 1 && deleted_count == 0);
    reentry_t events = {.candidate = root};
    assert(lv_obj_add_event_cb(root, event_reentry, LV_EVENT_SCREEN_UNLOADED, &events));
    assert(lv_obj_add_event_cb(root, event_reentry, LV_EVENT_DELETE, &events));
    lv_obj_t *replacement = system_root(0x00ff00, NULL);
    commit(replacement);
    assert(events.calls == 2 && deleted_count == 1);
    expect_pixel(0, 0, 0x07e0);
    assert(ryz_lvgl_system_release() == ESP_OK);
    passed("initial cancellation / unload / delete callback reentry cannot change lease");
}

static void repeated_release(void)
{
    lv_mem_monitor_t before, after;
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_mem_monitor(&before);
    const size_t external_bytes = ryz_host_heap_bytes();
    const size_t external_blocks = ryz_host_heap_blocks();
    unsigned deleted_count = 0;
    const ryz_ui_scene_t scene = lua_scene();
    for (unsigned i = 0; i < 64; ++i) {
        lv_obj_t *active = system_root(0xff0000, &deleted_count);
        rectangle(active, 10, 10, 30, 30, 0x00ff00);
        commit(active);
        expect_pixel(20, 20, 0x07e0);
        (void)system_root(0x0000ff, &deleted_count);
        assert(ryz_lvgl_system_release() == ESP_OK);
        assert(deleted_count == (i + 1U) * 2U);
        assert(ryz_lvgl_mount(&scene) == ESP_OK);
        ryz_lvgl_release();
        assert(lv_mem_test() == LV_RESULT_OK);
        lv_mem_monitor(&after);
        assert(after.free_size == before.free_size);
        assert(ryz_host_heap_bytes() == external_bytes);
        assert(ryz_host_heap_blocks() == external_blocks);
    }
    printf("LIFECYCLE_RESOURCES lv_pool_free=%zu shared_draw_bytes=%zu shared_draw_blocks=%zu\n",
           before.free_size, external_bytes, external_blocks);
    passed("64 system-candidate/active and Lua cycles return to the same real LVGL pool baseline");
}

static void system_release_wipes_retained_buffer(void)
{
    const size_t draw_bytes=240U*16U*sizeof(uint16_t);
    lv_obj_t *root=system_root(0xffffff,NULL);
    commit(root);
    lv_display_t *display=lv_display_get_default();
    lv_draw_buf_t *draw=lv_display_get_buf_active(display);
    assert(draw && draw->data && draw->data_size==draw_bytes);
    uint8_t *retained=draw->data;
    uint8_t before[240U*16U*sizeof(uint16_t)];
    memcpy(before,retained,sizeof(before));
    for(size_t i=0;i<draw_bytes;++i) assert(before[i]==0xff);
    const ryz_ui_scene_t scene=lua_scene();
    check_foreign(root,&scene);
    assert(!memcmp(before,retained,sizeof(before))); /* Rejected foreign release cannot erase. */
    const size_t shows=ryz_host_display_shows();
    assert(ryz_lvgl_system_release()==ESP_OK);
    assert(lv_display_get_buf_active(display)->data==retained);
    for(size_t i=0;i<draw_bytes;++i) assert(retained[i]==0);
    assert(ryz_host_display_shows()==shows); /* Erasing scratch is not a panel transfer. */
    expect_pixel(0,0,0xffff);

    assert(ryz_lvgl_mount(&scene)==ESP_OK);
    assert(lv_display_get_buf_active(display)->data==retained);
    memcpy(before,retained,sizeof(before));
    bool nonzero=false;
    for(size_t i=0;i<draw_bytes;++i) nonzero|=before[i]!=0;
    assert(nonzero);
    assert(ryz_lvgl_system_release()==ESP_ERR_INVALID_STATE);
    assert(!memcmp(before,retained,sizeof(before))); /* Lua owns these pixels. */
    check_foreign(lv_screen_active(),&scene);
    assert(!memcmp(before,retained,sizeof(before)));
    ryz_lvgl_release();
    memcpy(before,retained,sizeof(before));
    assert(ryz_lvgl_system_release()==ESP_OK);
    assert(!memcmp(before,retained,sizeof(before))); /* Ownerless is a strict no-op. */
    passed("successful system release wipes all retained draw bytes; Lua/foreign/no-lease paths do not");
}

static void lua_updates_in_place(void)
{
    ryz_ui_scene_t scene = lua_scene();
    assert(ryz_lvgl_mount(&scene) == ESP_OK);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *box = lv_obj_get_child(root, 0);
    lv_obj_t *title = lv_obj_get_child(root, 1);
    lv_obj_t *button = lv_obj_get_child(root, 2);
    lv_obj_t *button_label = lv_obj_get_child(button, 0);
    const char *title_storage = lv_label_get_text(title);
    const char *button_storage = lv_label_get_text(button_label);
    unsigned deletes = 0;
    assert(lv_obj_add_event_cb(root, deleted, LV_EVENT_DELETE, &deletes));
    scene.background = 0;
    scene.objects[0].background = 0xf800;
    strcpy(scene.objects[1].text, "UPDATED");
    strcpy(scene.objects[2].text, "STOP");
    scene.objects[2].background = 0xfb40; /* Ryzobee orange, RGB565. */
    scene.objects[2].foreground = 0;
    assert(ryz_lvgl_update(&scene) == ESP_OK);
    assert(!deletes && lv_screen_active() == root && lv_obj_get_child_count(root) == 3);
    assert(lv_obj_get_child(root, 0) == box && lv_obj_get_child(root, 1) == title);
    assert(lv_obj_get_child(root, 2) == button && lv_obj_get_child(button, 0) == button_label);
    assert(!strcmp(lv_label_get_text(title), "UPDATED"));
    assert(!strcmp(lv_label_get_text(button_label), "STOP"));
    assert(lv_label_get_text(title) == title_storage && lv_label_get_text(button_label) == button_storage);
    expect_pixel(0, 0, 0);
    expect_pixel(20, 20, 0xf800);
    expect_pixel(12, 82, 0xfb40);
    /* Source lifetime is the caller's: replacing its bytes cannot change the
     * mounted labels or their fixed storage. Restore before future updates. */
    strcpy(scene.objects[1].text, "CALLER MODIFIED");
    assert(!strcmp(lv_label_get_text(title), "UPDATED"));
    scene.objects[0].visible = scene.objects[1].visible = scene.objects[2].visible = false;
    assert(ryz_lvgl_update(&scene) == ESP_OK);
    expect_pixel(20, 20, 0);
    expect_pixel(12, 82, 0);
    assert(lv_obj_has_flag(box, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(title, LV_OBJ_FLAG_HIDDEN));
    scene.objects[0].visible = scene.objects[1].visible = scene.objects[2].visible = true;
    scene.objects[0].enabled = scene.objects[1].enabled = scene.objects[2].enabled = false;
    assert(ryz_lvgl_update(&scene) == ESP_OK);
    assert(!lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_bg_opa(box, 0) == LV_OPA_40);
    assert(lv_obj_get_style_text_opa(button_label, 0) == LV_OPA_40);
    scene.objects[0].enabled = scene.objects[1].enabled = scene.objects[2].enabled = true;
    assert(ryz_lvgl_update(&scene) == ESP_OK);
    assert(lv_obj_get_style_bg_opa(box, 0) == LV_OPA_COVER);
    assert(lv_obj_get_style_text_opa(button_label, 0) == LV_OPA_COVER);
    const size_t external_bytes = ryz_host_heap_bytes(), external_blocks = ryz_host_heap_blocks();
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    for (unsigned i = 0; i < 64; ++i) {
        strcpy(scene.objects[1].text, i % 2 ? "RX 00001234" : "RX 00005678");
        assert(ryz_lvgl_update(&scene) == ESP_OK);
        assert(lv_screen_active() == root && !deletes);
        assert(lv_label_get_text(title) == title_storage);
        assert(ryz_host_heap_bytes() == external_bytes && ryz_host_heap_blocks() == external_blocks);
        lv_mem_monitor(&after);
        assert(after.free_size == before.free_size && lv_mem_test() == LV_RESULT_OK);
    }
    ryz_lvgl_release();
    assert(deletes == 1);
    passed("Lua update preserves objects/static text across visibility/opacity and 64 bounded updates");
}

static void lua_update_admission(void)
{
    ryz_ui_scene_t scene = lua_scene();
    assert(ryz_lvgl_update(&scene) == ESP_ERR_INVALID_STATE);
    assert(ryz_lvgl_mount(&scene) == ESP_OK);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *title = lv_obj_get_child(root, 1);
    for (unsigned field = 0; field < 15; ++field) {
        ryz_ui_scene_t changed = scene;
        switch (field) {
        case 0: strcpy(changed.id, "new_scene"); break;
        case 1: ++changed.generation; break;
        case 2: --changed.object_count; break;
        case 3: strcpy(changed.objects[0].id, "new_box"); break;
        case 4: changed.objects[1].kind = RYZ_UI_BUTTON; break;
        case 5: ++changed.objects[0].x; break;
        case 6: ++changed.objects[1].y; break;
        case 7: ++changed.objects[1].width; break; /* BOX width is now mutable. */
        case 8: ++changed.objects[1].height; break;
        case 9: changed.objects[1].font = RYZ_UI_FONT_TITLE_24; break;
        case 10: changed.objects[1].align = RYZ_UI_ALIGN_RIGHT; break;
        case 11: ++changed.objects[0].radius; break;
        case 12: --changed.objects[0].border_width; break;
        case 13: changed.objects[0] = scene.objects[1]; changed.objects[1] = scene.objects[0]; break;
        case 14: changed.objects[1].text[0] = (char)0x80; break;
        }
        const size_t shows = ryz_host_display_shows();
        assert(ryz_lvgl_update(&changed) == (field == 14 ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE));
        assert(lv_screen_active() == root && !strcmp(lv_label_get_text(title), "RYZOBEE"));
        assert(ryz_host_display_shows() == shows);
    }
    scene.background = 0;
    unsigned cancellation_calls = 0;
    assert(ryz_lvgl_update_checked(&scene, cancel_now, &cancellation_calls) == ESP_ERR_TIMEOUT);
    expect_pixel(0, 0, 0xf800);
    assert(cancellation_calls == 1);
    reentry_t reentry = {.candidate = root};
    assert(ryz_lvgl_update_checked(&scene, cancellation_reentry, &reentry) == ESP_OK);
    assert(reentry.calls == 1 && lv_screen_active() == root);
    expect_pixel(0, 0, 0);
    ryz_lvgl_release();
    passed("Lua update rejects stale/layout/owner/system/reentry changes; early cancellation is untouched");
}

static void lua_update_failures(void)
{
    for (unsigned fault = 0; fault < 4; ++fault) {
#if !CONFIG_RYZ_LUA_FREETYPE
        if (fault == 3) continue; /* No glyph mutex exists in the static backend. */
#endif
        ryz_ui_scene_t scene = lua_scene();
        assert(ryz_lvgl_mount(&scene) == ESP_OK);
        lv_obj_t *root = lv_screen_active();
        scene.background = 0x001f;
        esp_err_t expected = ESP_ERR_TIMEOUT;
        transfer_cancel_t condition = {.after_rows = ryz_host_display_rows() + 16};
        if (fault == 0) { ryz_host_display_fail_blit(ESP_FAIL); expected = ESP_FAIL; }
        if (fault == 1) ryz_host_display_fail_show(ESP_ERR_TIMEOUT, 16);
        if (fault == 3) {
            strcpy(scene.objects[1].text, "~!@#$%^&*()_+");
            ryz_host_mutex_fail(true); /* Real glyph callbacks encounter a font-lock error. */
        }
        assert(ryz_lvgl_update_checked(&scene, fault == 2 ? cancel_after_transfer : NULL,
                                        &condition) == expected);
        ryz_host_mutex_fail(false);
        assert(lv_screen_active() == root);
        if (fault == 1 || fault == 2) {
            expect_pixel(0, 0, 0x001f);
            expect_pixel(0, 150, 0xf800); /* No fictional physical rollback. */
        }
        const size_t shows = ryz_host_display_shows();
        assert(ryz_lvgl_update(&scene) == ESP_ERR_INVALID_STATE);
        assert(ryz_lvgl_pump() == ESP_ERR_INVALID_STATE);
        assert(ryz_host_display_shows() == shows);
        assert(ryz_lvgl_mount(&scene) == ESP_OK);
        expect_pixel(0, 150, 0x001f);
        ryz_lvgl_release();
    }
    /* Every external allocation site in the new scene/font storage is bounded
     * and retryable. Old objects remain referenced until replacement succeeds. */
    ryz_ui_scene_t scene = lua_scene();
    assert(ryz_lvgl_mount(&scene) == ESP_OK);
    lv_obj_t *root = lv_screen_active();
    const size_t bytes = ryz_host_heap_bytes(), blocks = ryz_host_heap_blocks();
    for (size_t allocation = 0; allocation <
#if CONFIG_RYZ_LUA_FREETYPE
         5
#else
         1 /* Only scene storage; static glyphs allocate nothing. */
#endif
         ; ++allocation) {
        ryz_host_heap_fail_after(allocation);
        assert(ryz_lvgl_mount(&scene) == ESP_ERR_NO_MEM);
        ryz_host_heap_allow();
        assert(lv_screen_active() == root);
        assert(ryz_host_heap_bytes() == bytes && ryz_host_heap_blocks() == blocks);
        assert(ryz_lvgl_update(&scene) == ESP_OK);
    }
    ryz_lvgl_release();
    passed("Lua update returns real flush/font errors, taints partial trees, remount repairs and allocation failures unwind");
}

static void check_mapped_font(lv_obj_t *label, ryz_font_face_t face, uint16_t size)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    assert(font && font != &lv_font_montserrat_16 && font != &lv_font_montserrat_24 && !font->fallback);
#if CONFIG_RYZ_LUA_FREETYPE
    ryz_font_face_info_t info;
    assert(ryz_font_get_face_info(face, &info) == ESP_OK);
    assert(!strcmp(info.family, "Noto Sans") && info.weight == (size == 24 ? 600 : 400));
    for (uint32_t code = 0x20; code <= 0x7e; ++code) {
        ryz_font_glyph_t expected;
        esp_err_t error = ryz_font_rasterize_ascii(face, code, size, NULL, 0, &expected);
        assert(error == ESP_OK || error == ESP_ERR_INVALID_SIZE);
        lv_font_glyph_dsc_t actual;
        assert(lv_font_get_glyph_dsc(font, &actual, code, 0));
        assert(actual.resolved_font == font && actual.adv_w == expected.advance_x);
        assert(actual.box_w == expected.width && actual.box_h == expected.height);
        assert(actual.ofs_x == expected.bearing_x && actual.ofs_y == expected.bearing_y - expected.height);
    }
#else
    const lv_font_t *expected = ryz_v5_font_get(face, size);
    assert(expected && font->static_bitmap && font->dsc == expected->dsc);
    assert(font->line_height == expected->line_height && font->base_line == expected->base_line);
    assert(!ryz_font_ready());
#endif
}

static void lua_brand_pixels(const char *directory)
{
    ryz_ui_scene_t scene = {.id = "font_probe", .generation = 17, .object_count = 6};
    scene.objects[0] = (ryz_ui_object_t){.id = "title", .kind = RYZ_UI_LABEL,
        .x = 12, .y = 8, .width = 216, .height = 36, .text = "FONT / DEMO",
        .foreground = 0xffff, .font = RYZ_UI_FONT_TITLE_24, .visible = true, .enabled = true};
    scene.objects[1] = (ryz_ui_object_t){.id = "caption", .kind = RYZ_UI_LABEL,
        .x = 12, .y = 50, .width = 216, .height = 26, .text = "Noto Sans / 16px",
        .foreground = 0xa514, .visible = true, .enabled = true};
    scene.objects[2] = (ryz_ui_object_t){.id = "value", .kind = RYZ_UI_LABEL,
        .x = 12, .y = 90, .width = 216, .height = 26, .text = "RX 00000000",
        .foreground = 0xffff, .visible = true, .enabled = true};
    scene.objects[3] = (ryz_ui_object_t){.id = "status", .kind = RYZ_UI_LABEL,
        .x = 12, .y = 124, .width = 216, .height = 26, .text = "STATUS: RUNNING",
        .foreground = 0xa514, .visible = true, .enabled = true};
    scene.objects[4] = (ryz_ui_object_t){.id = "pause", .kind = RYZ_UI_BUTTON,
        .x = 12, .y = 174, .width = 100, .height = 44, .text = "PAUSE",
        .background = 0xfb40, .border = 0xfb40, .border_width = 1,
        .align = RYZ_UI_ALIGN_CENTER, .visible = true, .enabled = true};
    scene.objects[5] = (ryz_ui_object_t){.id = "stop", .kind = RYZ_UI_BUTTON,
        .x = 128, .y = 174, .width = 100, .height = 44, .text = "STOP",
        .foreground = 0xffff, .border = 0x39c7, .border_width = 1,
        .align = RYZ_UI_ALIGN_CENTER, .visible = true, .enabled = true};
    assert(ryz_lvgl_mount(&scene) == ESP_OK);
    lv_obj_t *root = lv_screen_active();
    check_mapped_font(lv_obj_get_child(root, 0), RYZ_FONT_BODY_SEMIBOLD, 24);
    check_mapped_font(lv_obj_get_child(root, 1), RYZ_FONT_BODY, 16);
    check_mapped_font(lv_obj_get_child(lv_obj_get_child(root, 4), 0), RYZ_FONT_BODY, 16);
    ryz_host_display_save(directory, "lua-update-before-240");
    strcpy(scene.objects[2].text, "RX 00001234");
    strcpy(scene.objects[3].text, "STATUS: PAUSED");
    strcpy(scene.objects[4].text, "RESUME");
    scene.objects[5].enabled = false;
    assert(ryz_lvgl_update(&scene) == ESP_OK && lv_screen_active() == root);
    ryz_host_display_save(directory, "lua-update-after-240");
    assert(count_pixels(12, 8, 216, 36, 0) > 300);
    assert(count_pixels(12, 50, 216, 26, 0) > 100);
    ryz_lvgl_release();
    passed("real Noto400/16 and Noto600/24 mapping, 95 ASCII metrics, before/after 240x240 RGB565 export");
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    ryz_host_display_reset();
    bootstrap();
    assert(ryz_font_init() == ESP_OK);
    system_pixels_and_ownership();
    legacy_wrapper();
    failures_and_cancellation();
    callback_guards();
    repeated_release();
    system_release_wipes_retained_buffer();
    lua_updates_in_place();
    lua_update_admission();
    lua_update_failures();
    lua_brand_pixels(argc == 2 ? argv[1] : ".");
    printf("LVGL_LIFECYCLE_PASS cases=%u\n", cases);
    return 0;
}
