/* Real original-V5 UI + LVGL/FreeType. Only the copy/intent boundary below is
 * controlled. Real Store/RPC/Job behavior is tested by workbench_apps_test;
 * this diagnostic does not simulate hardware or claim on-panel readability. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ryz_v5_apps.h"
#include "ryz_v5_font.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_assets.h"
#include "ryz_v5_render.h"
#include "ryz_v5_status.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"

#ifdef NDEBUG
#error "V5 UI checks require assertions"
#endif

static ryz_v5_apps_snapshot_t state;
static ryz_v5_apps_intent_t actions[32];
static unsigned calls, cases;
static esp_err_t get_error, submit_error;

static esp_err_t get(void *context, ryz_v5_apps_snapshot_t *out)
{
    assert(context == &state);
    memset(out, 0, sizeof(*out));
    if (get_error == ESP_OK) *out = state;
    return get_error;
}

static esp_err_t submit(void *context, const ryz_v5_apps_intent_t *intent)
{
    assert(context == &state && calls < 32);
    actions[calls++] = *intent;
    return submit_error;
}

static void up(uint64_t time)
{
    const ryz_touch_sample_t sample = {.event = RYZ_TOUCH_UP};
    (void)ryz_v5_apps_pointer(&sample, time);
}

static void gesture_up(uint8_t gesture, uint64_t time)
{
    const ryz_touch_sample_t sample = {.event = RYZ_TOUCH_UP, .gesture = gesture};
    (void)ryz_v5_apps_pointer(&sample, time);
}

static void down(unsigned x, unsigned y, uint64_t time)
{
    const ryz_touch_sample_t sample = {.pressed = true, .has_position = true,
        .x = (uint16_t)x, .y = (uint16_t)y, .event = RYZ_TOUCH_DOWN};
    assert(ryz_v5_apps_pointer(&sample, time));
}

static void move(unsigned x, unsigned y, uint64_t time)
{
    const ryz_touch_sample_t sample = {.pressed = true, .has_position = true,
        .x = (uint16_t)x, .y = (uint16_t)y, .event = RYZ_TOUCH_MOVE};
    assert(ryz_v5_apps_pointer(&sample, time));
}

static void stream_fixture(void)
{
    state = (ryz_v5_apps_snapshot_t){.open = true, .page_limit = RYZ_SCRIPT_STORE_PAGE_MAX,
        .token = {.page = {.generation = 1, .route = 2}, .reader_request_id = 7}};
    state.reader = (ryz_apps_snapshot_t){.request_id = 7, .completed_id = 7,
        .view = RYZ_APPS_VIEW_CATALOG, .state = RYZ_APPS_READY,
        .store_status_valid = true, .store_ready = true};
    state.reader.page.revision = 9;
    state.reader.page.total = 8;
    state.reader.page.count = 8;
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(state.reader.page.entries[i].name,
                 sizeof(state.reader.page.entries[i].name), "app%u.lua", i);
        state.reader.page.entries[i].bytes = 2048 + i;
    }
    get_error = submit_error = ESP_OK;
    calls = 0;
    const ryz_v5_apps_binding_t binding = {.get = get, .submit = submit, .context = &state};
    ryz_v5_apps_bind(&binding);
    assert(ryz_v5_apps_tick(0));
}

static void catalog(void)
{
    stream_fixture();
    up(0);
    down(70, 55, 10);
    up(20);
    (void)ryz_v5_apps_tick(20);
    assert(calls == 0 && !strcmp(ryz_v5_apps_title(), "APPS"));
}

static void detail(const char *name, uint32_t revision)
{
    state.token.reader_request_id++;
    memset(&state.reader, 0, sizeof(state.reader));
    state.reader.request_id = state.token.reader_request_id;
    state.reader.completed_id = state.token.reader_request_id;
    state.reader.view = RYZ_APPS_VIEW_DETAIL;
    state.reader.state = RYZ_APPS_READY;
    state.reader.store_status_valid = true;
    state.reader.store_ready = true;
    state.reader.detail.revision = revision;
    state.reader.detail.source_valid = true;
    state.reader.detail.file.entry.bytes = 1024;
    snprintf(state.reader.detail.file.entry.name, sizeof(state.reader.detail.file.entry.name), "%s", name);
    memset(state.reader.detail.file.sha256, 'a', 64);
    state.reader.detail.file.sha256[64] = 0;
    strcpy(state.reader.detail.metadata.author, "RYZOBEE");
    strcpy(state.reader.detail.metadata.version, "1.0.0");
    strcpy(state.reader.detail.metadata.description, "Original V5 application description wraps across two lines.");
}

static void passed(const char *name) { ++cases; printf("PASS %s\n", name); }

static void click_and_hold(void)
{
    catalog();
    down(70, 100, 100);
    up(200);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT && actions[0].index == 1);
    assert(actions[0].expected.reader_request_id == 7);

    catalog();
    down(70, 70, 100);
    (void)ryz_v5_apps_tick(3099);
    assert(calls == 0);
    (void)ryz_v5_apps_tick(3100);
    assert(calls == 0);
    up(3101);
    assert(calls == 0); /* List hold is neither selection nor execution. */
    detail("app0.lua", 9);
    (void)ryz_v5_apps_tick(3110);
    down(100, 198, 3200); /* Full 44px hit area includes above the visual button. */
    (void)ryz_v5_apps_tick(5199); assert(calls == 0);
    (void)ryz_v5_apps_tick(5200);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_RUN);
    assert(actions[0].expected.reader_request_id == state.token.reader_request_id);
    (void)ryz_v5_apps_tick(9000); up(9001);
    down(100, 220, 9100); (void)ryz_v5_apps_tick(13000); up(13001);
    assert(calls == 1); /* Waiting for the Owner cannot arm a duplicate. */
    passed("list tap selects; list hold does nothing; detail 2000ms hold submits exactly once");
}

static void gesture_cancel(void)
{
    catalog();
    down(70, 70, 100);
    move(79, 79, 150); /* Euclidean movement >12px, not just axis distance. */
    (void)ryz_v5_apps_tick(1500);
    up(1501);
    assert(calls == 0);
    down(70, 83, 2000);
    move(70, 85, 2010); /* Leave row even before 12px. */
    (void)ryz_v5_apps_tick(4000);
    up(4001);
    assert(calls == 0);
    down(70, 100, 5000);
    move(70, 80, 5010);
    assert(calls == 0); /* Twenty pixels, not an asynchronous four-row jump. */
    (void)ryz_v5_apps_tick(7000);
    up(7001);
    assert(calls == 0);
    down(70,70,7100); up(7120);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT && actions[0].index == 1);
    passed("move/leave target cancel permanently; vertical dragging uses continuous pixel offset");
}

static void interrupted_contacts_cancel(void)
{
    /* NONE means the controller observed release without a trustworthy UP;
     * a repeated DOWN means the stream is discontinuous. Neither can finish
     * the old contact, even if the coordinates are unchanged. */
    for (unsigned duplicate = 0; duplicate < 6; ++duplicate) {
        for (unsigned positioned = 0; positioned < 2; ++positioned) {
            catalog();
            down(120, 70, 100);
            const ryz_touch_sample_t interrupted = {
                .event = duplicate > 1 ? RYZ_TOUCH_UP : duplicate ? RYZ_TOUCH_DOWN : RYZ_TOUCH_NONE,
                .pressed = duplicate == 1, .gesture = duplicate > 1 ? duplicate - 1 : 0,
                .has_position = duplicate || positioned,
                .x = 120, .y = 70,
            };
            assert(ryz_v5_apps_pointer(&interrupted, 110));
            up(120);
            assert(calls == 0);
            down(120, 70, 130); up(150);
            assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT);
        }
        const unsigned xs[] = {20, 30, 160, 120};
        const unsigned ys[] = {16, 210, 204, 218};
        for (unsigned target = 0; target < 4; ++target) {
            catalog(); detail("app0.lua", 9); (void)ryz_v5_apps_tick(0);
            if (target == 2) { down(30, 210, 10); up(20); }
            down(xs[target], ys[target], 100);
            const ryz_touch_sample_t interrupted = {
                .event = duplicate > 1 ? RYZ_TOUCH_UP : duplicate ? RYZ_TOUCH_DOWN : RYZ_TOUCH_NONE,
                .pressed = duplicate == 1, .has_position = true,
                .gesture = duplicate > 1 ? duplicate - 1 : 0,
                .x = (uint16_t)xs[target], .y = (uint16_t)ys[target],
            };
            assert(ryz_v5_apps_pointer(&interrupted, 110));
            (void)ryz_v5_apps_tick(3100);
            up(3110);
            assert(calls == 0);
            assert(!strcmp(ryz_v5_apps_title(), target == 2 ? "DELETE APP" : "APP INFO"));
        }
    }
    passed("lost UP and duplicate DOWN revoke row, Back, delete and RUN captures without click-through");
}

/* Synthetic input replays through the production Apps controller, not a
 * captured panel trace. CST816 release samples have no trustworthy position;
 * a direction marker must veto row selection even when no MOVE was observed.
 * The marker is deliberately not translated into screen navigation here. */
static void release_gesture_cancels_row(void)
{
    for (uint8_t gesture = 1; gesture <= 4; ++gesture) {
        for (unsigned jitter = 0; jitter < 2; ++jitter) {
            catalog();
            down(120, 70, 100);
            if (jitter) move(122, 72, 110);
            gesture_up(gesture, 120);
            assert(calls == 0 && ryz_v5_apps_has_navigation());
            gesture_up(gesture, 130);
            up(140);
            assert(calls == 0); /* A duplicate release cannot resurrect a row. */
            down(120, 70, 150); up(170);
            assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT &&
                   actions[0].index == 0);
        }
    }
    passed("all four direction-only releases cancel rows, with or without jitter; a new contact can select");
}

static void release_tap_markers_select_once(void)
{
    const uint8_t gestures[] = {0, 5};
    for (size_t i = 0; i < sizeof(gestures) / sizeof(gestures[0]); ++i) {
        catalog();
        down(120, 100, 100);
        move(122, 102, 110);
        gesture_up(gestures[i], 120);
        assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT &&
               actions[0].index == 1);
        gesture_up(gestures[i], 130);
        up(140);
        (void)ryz_v5_apps_tick(150);
        assert(calls == 1);
    }
    passed("unmarked and single-click releases still select exactly once after small in-row jitter");
}

static void row_drag_stays_cancelled(void)
{
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        catalog();
        down(120, 130, 100);
        move(120, 101, 110); /* One row of vertical scrolling. */
        if (reverse) move(120, 130, 120);
        up(130);
        up(140);
        assert(calls == 0);
        down(120, 70, 150); up(170);
        assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT &&
               actions[0].index == (reverse ? 0U : 1U));
    }

    catalog();
    down(120, 70, 100);
    move(90, 70, 110);
    move(120, 70, 120);
    gesture_up(5, 130); /* Returning to the start never re-arms a dragged row. */
    assert(calls == 0);
    down(120, 70, 150); up(170);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT &&
           actions[0].index == 0);
    passed("vertical drag preserves list position; vertical and horizontal return paths never become row taps");
}

static void row_snapshot_change_cancels(void)
{
    for (unsigned change = 0; change < 4; ++change) {
        catalog();
        down(120, 70, 100);
        if (change == 0) {
            ++state.token.reader_request_id;
            state.reader.request_id = state.reader.completed_id = state.token.reader_request_id;
        } else if (change == 1) {
            ++state.reader.page.revision;
        } else if (change == 2) {
            strcpy(state.reader.page.entries[0].name, "replacement.lua");
        } else {
            detail("app0.lua", 9);
        }
        (void)ryz_v5_apps_tick(110);
        gesture_up(5, 120);
        up(130);
        (void)ryz_v5_apps_tick(4000);
        assert(calls == 0); /* Neither stale selection nor a delayed RUN. */
    }
    passed("reader identity, revision, row replacement and incoming metadata cannot complete an old row capture");
}

static void stale_and_release(void)
{
    catalog();
    down(70, 70, 100);
    ++state.token.reader_request_id;
    (void)ryz_v5_apps_tick(1500);
    up(1501);
    assert(calls == 0);
    down(70, 70, 2000); up(2020);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT);
    detail("changed.lua", 9);
    (void)ryz_v5_apps_tick(5010);
    assert(calls == 1);

    catalog();
    const ryz_apps_snapshot_t restored_catalog = state.reader;
    down(70, 70, 100); up(120);
    assert(calls == 1);
    ryz_v5_apps_reset();
    detail("app0.lua", 9);
    (void)ryz_v5_apps_tick(1400);
    assert(calls == 2 && actions[1].action == RYZ_V5_APPS_BACK);
    assert(!strcmp(ryz_v5_apps_title(), "APPS"));
    down(120,210,1500); (void)ryz_v5_apps_tick(3000);
    up(3100);
    down(70,55,3200); (void)ryz_v5_apps_tick(4400); up(4401);
    assert(calls == 2); /* One recovery read; no late hold/run/retry. */
    state.reader = restored_catalog; ++state.token.reader_request_id;
    state.reader.request_id = state.reader.completed_id = state.token.reader_request_id;
    (void)ryz_v5_apps_tick(4500);
    detail("app0.lua", 9); (void)ryz_v5_apps_tick(4510);
    down(120, 210, 4600); (void)ryz_v5_apps_tick(7600); up(7601);
    assert(calls == 3 && actions[2].action == RYZ_V5_APPS_RUN);
    passed("changed reader/name discard stale run; reset keeps late detail behind stream and requires release before an explicit browser BACK request");
}

static void deletion(void)
{
    catalog(); detail("app0.lua", 9); (void)ryz_v5_apps_tick(0);
    down(30, 210, 100); up(120);
    assert(!strcmp(ryz_v5_apps_title(), "DELETE APP") && calls == 0);
    down(50, 204, 130); up(150);
    assert(!strcmp(ryz_v5_apps_title(), "APP INFO") && calls == 0);
    down(30, 210, 200); up(220);
    down(160, 204, 230); up(250);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_DELETE);
    assert(!strcmp(actions[0].delete_name, "app0.lua"));
    assert(!strcmp(actions[0].delete_sha256, state.reader.detail.file.sha256));
    state.delete_state = RYZ_V5_APPS_DELETE_UNKNOWN;
    (void)ryz_v5_apps_tick(260);
    down(120, 210, 300); (void)ryz_v5_apps_tick(1500); up(1501);
    assert(calls == 1); /* Do not repeat an uncertain action in the same view. */
    detail("app1.lua", 10); (void)ryz_v5_apps_tick(1510);
    down(120, 210, 1600); (void)ryz_v5_apps_tick(4600); up(4601);
    assert(calls == 2 && actions[1].action == RYZ_V5_APPS_RUN);

    catalog(); detail("app0.lua", 9); (void)ryz_v5_apps_tick(0);
    down(30, 210, 100); up(120);
    state.reader.detail.file.sha256[0] = 'b'; /* Same reader ID cannot substitute the dialog's hash. */
    (void)ryz_v5_apps_tick(125);
    up(126);
    down(160, 204, 130); up(150);
    assert(calls == 0 && strcmp(ryz_v5_apps_title(), "DELETE APP"));
    state.reader.detail.file.entry.protected_file = true;
    (void)ryz_v5_apps_tick(160);
    down(30, 210, 170); up(190);
    assert(calls == 0 && strcmp(ryz_v5_apps_title(), "DELETE APP"));
    passed("delete requires distinct confirmation, exact original hash, and an unprotected file");
}

static void errors_and_back(void)
{
    catalog(); detail("app0.lua", 9); (void)ryz_v5_apps_tick(0);
    state.reader.detail.source_valid = false;
    (void)ryz_v5_apps_tick(10);
    down(120, 210, 100); (void)ryz_v5_apps_tick(1400); up(1401);
    assert(calls == 0);
    down(20, 16, 1500); up(1510);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_BACK);

    catalog();
    submit_error = ESP_ERR_TIMEOUT;
    down(70, 70, 100); up(120);
    assert(calls == 1);
    (void)ryz_v5_apps_tick(5000);
    assert(calls == 1); /* A busy submit is not automatically retried. */
    get_error = ESP_ERR_INVALID_STATE;
    (void)ryz_v5_apps_tick(5010);
    down(70, 70, 5100); (void)ryz_v5_apps_tick(7000); up(7001);
    assert(calls == 1);
    passed("invalid source cannot run; back works; busy/offline do not auto-retry");
}

static uint16_t rgb565(unsigned rgb)
{ return (uint16_t)(((rgb >> 19) << 11) | (((rgb >> 10) & 63) << 5) | ((rgb >> 3) & 31)); }

static bool has_label(lv_obj_t *root, const char *text)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *child = lv_obj_get_child(root, (int32_t)i);
        if (lv_obj_check_type(child, &lv_label_class) && !strcmp(lv_label_get_text(child), text)) return true;
        if (has_label(child, text)) return true;
    }
    return false;
}

static lv_obj_t *find_text(lv_obj_t *root,const char *prefix)
{
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if(lv_obj_check_type(child,&lv_label_class) && !strncmp(lv_label_get_text(child),prefix,strlen(prefix))) return child;
        lv_obj_t *nested=find_text(child,prefix);
        if(nested) return nested;
    }
    return NULL;
}

static lv_obj_t *render(void)
{
    lv_obj_t *root = NULL;
    assert(ryz_lvgl_system_create(&root) == ESP_OK && root);
    ryz_v5_widgets_begin();
    assert(ryz_v5_apps_draw(root) == ESP_OK);
    assert(ryz_v5_widgets_status() == ESP_OK);
    bool cancelled = true;
    assert(ryz_lvgl_system_commit(root, NULL, NULL, &cancelled) == ESP_OK && !cancelled);
    assert(ryz_v5_widgets_status() == ESP_OK);
    return root;
}

static lv_obj_t *render_full(void)
{
    const ryz_system_ui_snapshot_t system = {.time_hhmm = "18:43"};
    const ryz_v5_status_t views = {0};
    (void)ryz_v5_status_set(&views);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS, &system, NULL, NULL, NULL) == ESP_OK);
    assert(ryz_v5_widgets_status() == ESP_OK);
    return lv_screen_active();
}

static void hold_feedback_cost(const char *directory)
{
    catalog(); detail("latency.lua",9); (void)ryz_v5_apps_tick(20);
    lv_obj_t *root=render_full();
    uint16_t original[240*240];
    memcpy(original,ryz_host_display_frame(),sizeof(original));
    size_t worst=0;
    const char *names[]={"hold-idle","hold-down","hold-progress","hold-release"};
    if(directory) ryz_host_display_save(directory,names[0]);
    for(unsigned i=1;i<4;++i) {
        if(i==1) down(120,218,100);
        else if(i==2) assert(ryz_v5_apps_tick(1600));
        else up(1610);
        const size_t before=ryz_host_display_blit_pixels();
        struct timespec begin,end;
        clock_gettime(CLOCK_MONOTONIC,&begin);
        assert(render_full()==root);
        clock_gettime(CLOCK_MONOTONIC,&end);
        const size_t pixels=ryz_host_display_blit_pixels()-before;
        const double ms=(end.tv_sec-begin.tv_sec)*1000.0+(end.tv_nsec-begin.tv_nsec)/1000000.0;
        printf("HOLD_FEEDBACK %s pixels=%zu host_ms=%.3f\n",names[i],pixels,ms);
        fflush(stdout);
        if(pixels>worst) worst=pixels;
        if(directory) ryz_host_display_save(directory,names[i]);
        for(unsigned y=0;y<240;++y) for(unsigned x=0;x<240;++x) {
            if(x>=80 && x<232 && y>=200 && y<236) continue;
            assert(ryz_host_display_pixel(x,y)==original[y*240+x]);
        }
    }
    assert(!memcmp(original,ryz_host_display_frame(),sizeof(original)));
    const size_t idle_pixels=ryz_host_display_blit_pixels();
    assert(render_full()==root);
    assert(ryz_host_display_blit_pixels()==idle_pixels);
    assert(calls==0); /* Releasing before 2 seconds never runs the script. */
    /* LVGL reserves 5 extra pixels at each label edge for glyph overhang.
     * Permit that rasterizer margin, not unrelated metadata/header pixels. */
    assert(worst<=162U*36U);
    assert(ryz_v5_release()==ESP_OK);
    passed("RUN press/progress/release redraw only the button, preserving the 2 second gate");
}

static bool cancel_feedback(void *context) { return *(bool *)context; }

static void hold_feedback_invalidation(void)
{
    catalog(); detail("latency.lua",9); (void)ryz_v5_apps_tick(20);
    (void)render_full(); down(120,218,100); (void)render_full();
    assert(ryz_v5_apps_tick(1000)); (void)render_full();
    size_t before=ryz_host_display_blit_pixels();
    assert(ryz_v5_apps_tick(1030)); (void)render_full();
    assert(ryz_host_display_blit_pixels()-before<=162U*36U); /* Background fill stays local. */
    /* Metadata changes force a full draw and revoke a stale armed hold. */
    ++state.reader.detail.revision;
    strcpy(state.reader.detail.metadata.author,"UPDATED");
    assert(ryz_v5_apps_tick(1040)); before=ryz_host_display_blit_pixels();
    assert(has_label(render_full(),"UPDATED"));
    assert(ryz_host_display_blit_pixels()-before==240U*240U);
    (void)ryz_v5_apps_tick(4000); up(4010); assert(calls==0);
    (void)render_full();
    before=ryz_host_display_blit_pixels();
    ryz_v5_apps_scroll(1000); (void)render_full(); up(4020);
    assert(ryz_host_display_blit_pixels()>before);
    assert(ryz_host_display_blit_pixels()-before<240U*240U);
    ryz_system_ui_snapshot_t system={.time_hhmm="18:44"};
    before=ryz_host_display_blit_pixels();
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_host_display_blit_pixels()-before==240U*240U);
    assert(has_label(lv_screen_active(),"18:44"));
    down(120,218,4100);
    bool cancel=true,observed=false;
    before=ryz_host_display_blit_pixels();
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,cancel_feedback,&cancel,&observed)==ESP_ERR_TIMEOUT);
    assert(observed && before==ryz_host_display_blit_pixels());
    cancel=false;
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,cancel_feedback,&cancel,&observed)==ESP_OK);
    assert(!observed && has_label(lv_screen_active(),"HOLD 0.0 / 2S"));
    up(4110);
    ryz_host_display_fail_blit(ESP_FAIL);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,NULL,NULL,NULL)==ESP_FAIL);
    before=ryz_host_display_blit_pixels();
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_host_display_blit_pixels()-before==240U*240U);
    assert(has_label(lv_screen_active(),"HOLD 2S TO RUN"));
    /* Direct lease release invalidates all borrowed widgets, too. */
    assert(ryz_lvgl_system_release()==ESP_OK);
    before=ryz_host_display_blit_pixels();
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_host_display_blit_pixels()-before==240U*240U);
    assert(ryz_v5_release()==ESP_OK && calls==0);
    passed("RUN fast path preserves metadata/header invalidation, local scrolling, cancellation, failure repair and root lifetime");
}

static void row_feedback_waits_for_stable_contact(void)
{
    assert(ryz_font_init() == ESP_OK);
    catalog();
    assert(!ryz_v5_apps_tick(90));
    down(120, 100, 100); /* Second row distinguishes feedback from the anchor. */
    assert(!ryz_v5_apps_tick(100));
    (void)render_full(); /* An unrelated repaint must not bypass the delay. */
    assert(ryz_host_display_pixel(8, 91) == rgb565(0x3a3a3a));
    assert(!ryz_v5_apps_tick(179));
    (void)render_full();
    assert(ryz_host_display_pixel(8, 91) == rgb565(0x3a3a3a));
    assert(ryz_v5_apps_tick(180));
    (void)render_full();
    assert(ryz_host_display_pixel(8, 91) == rgb565(0xff6a00));
    assert(!ryz_v5_apps_tick(181) && !ryz_v5_apps_tick(200));
    up(210);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT && actions[0].index == 1);

    catalog();
    down(120, 100, 100);
    move(150, 100, 120);
    assert(ryz_v5_apps_tick(120)); /* Consume the movement invalidation. */
    assert(!ryz_v5_apps_tick(179) && !ryz_v5_apps_tick(180));
    (void)render_full();
    assert(ryz_host_display_pixel(8, 91) == rgb565(0x3a3a3a));
    up(190);
    assert(calls == 0);

    catalog();
    down(120, 100, 100);
    assert(ryz_v5_apps_tick(180));
    ryz_v5_apps_cancel_gesture();
    assert(ryz_v5_apps_tick(190));
    up(191);
    down(120, 100, 200);
    assert(!ryz_v5_apps_tick(200) && !ryz_v5_apps_tick(279));
    (void)render_full();
    assert(ryz_host_display_pixel(8, 91) == rgb565(0x3a3a3a));
    assert(ryz_v5_apps_tick(280));
    (void)render_full();
    assert(ryz_host_display_pixel(8, 91) == rgb565(0xff6a00));
    assert(!ryz_v5_apps_tick(281));
    up(290);
    assert(calls == 1 && actions[0].action == RYZ_V5_APPS_SELECT && actions[0].index == 1);
    assert(ryz_v5_release() == ESP_OK);
    passed("row feedback starts once at 80ms, never on DOWN or after movement; cancellation resets both timer and pixels");
}

static lv_obj_t *find_label_at(lv_obj_t *root, const char *text, int x, int top, int width, int height)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *object = lv_obj_get_child(root, (int32_t)i);
        lv_obj_t *nested = find_label_at(object, text, x, top, width, height);
        if (nested) return nested;
        lv_area_t bounds; lv_obj_get_coords(object, &bounds);
        if (!lv_obj_check_type(object, &lv_label_class) ||
            strcmp(lv_label_get_text(object), text) || bounds.x1 != x) continue;
        const int center = 2 * bounds.y1 + lv_obj_get_height(object);
        if (abs(center - (2 * top + height)) > 1) continue;
        assert(lv_obj_get_width(object) == width);
        return object;
    }
    return NULL;
}

static lv_obj_t *label_at(lv_obj_t *root, const char *text, int x, int top, int width, int height)
{
    lv_obj_t *found = find_label_at(root, text, x, top, width, height);
    assert(found && "missing original V5 label geometry");
    return found;
}

static bool has_back_asset(lv_obj_t *root)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *object = lv_obj_get_child(root, (int32_t)i);
        if (lv_obj_check_type(object, &lv_image_class) &&
            lv_image_get_src(object) == &ryz_v5_asset_back) return true;
    }
    return false;
}

/* One glyph per new face/size choice checks actual weight, not just identical
 * line heights. The complete font rasterizer contract has its own test. */
static void label_font(lv_obj_t *object, ryz_font_face_t face, unsigned px)
{
    const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
#if CONFIG_RYZ_LUA_FREETYPE
    ryz_font_metrics_t metrics;
    assert(ryz_font_get_metrics(face, px, &metrics) == ESP_OK);
    assert(font && font->line_height == metrics.line_height && font->base_line == -metrics.descender);
    uint8_t expected[1024];
    ryz_font_glyph_t glyph;
    assert(ryz_font_rasterize_ascii(face, 'M', px, expected, sizeof(expected), &glyph) == ESP_OK);
    lv_font_glyph_dsc_t actual;
    assert(lv_font_get_glyph_dsc(font, &actual, 'M', 0));
    assert(actual.box_w == glyph.width && actual.box_h == glyph.height && actual.adv_w == glyph.advance_x);
    lv_draw_buf_t *buffer = lv_draw_buf_create(actual.box_w, actual.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    assert(buffer && lv_font_get_glyph_bitmap(&actual, buffer) == buffer);
    for (unsigned y = 0; y < glyph.height; ++y)
        assert(!memcmp(buffer->data + y * buffer->header.stride, expected + y * glyph.width, glyph.width));
    lv_font_glyph_release_draw_data(&actual);
    lv_draw_buf_destroy(buffer);
#else
    /* Full glyph/metric equivalence to FreeType is independently checked by
     * v5_static_font_test, linked only to the offline oracle. Product tests
     * must not turn the engine back on to inspect static font selection. */
    const lv_font_t *expected = ryz_v5_font_get(face, px);
    assert(expected && font == expected && font->static_bitmap);
    assert(!ryz_font_ready());
#endif
}

static void stream_root(const char *directory)
{
    assert(ryz_font_init() == ESP_OK);
    stream_fixture(); up(0);
    lv_obj_t *root = render_full();
    assert(has_label(root, "APPS") && has_label(root, "RYZOBEE"));
    assert(has_label(root, "LUA FILES") && has_label(root, "08 TOTAL"));
    assert(!has_label(root, "TAP INFO · HOLD 3S"));
    /* Offscreen rows now remain in the bounded reader-window pool. Their
     * existence is not visibility: clipping must still exclude row 5. */
    assert(lv_obj_is_visible(find_text(root,"app4.lua")));
    assert(!lv_obj_is_visible(find_text(root,"app5.lua")));
    assert(!has_label(root, "LUA TOOL") && !has_label(root, "LOCKED"));
    assert(!has_back_asset(root) && ryz_v5_apps_has_navigation() && calls == 0);
    for (unsigned i = 0; i < 4; ++i) {
        int y = 57 + (int)i * 29;
        lv_obj_t *name = label_at(root, state.reader.page.entries[i].name, 42, y+4, 106, 20);
        label_font(name, RYZ_FONT_BODY_SEMIBOLD, 12);
        label_font(label_at(root,"LUA",15,y+5,22,18),RYZ_FONT_MONO_MEDIUM,12);
        assert(ryz_host_display_pixel(8,y+5) == rgb565(i ? 0x3a3a3a : 0xff6a00));
        assert(ryz_host_display_pixel(230,y+12) == rgb565(i ? 0x181818 : 0x242424));
    }
    assert(has_label(root, "HOME") && has_label(root, "SETTING"));
    assert(ryz_host_display_pixel(120,239) == rgb565(0xff6a00));
    if(directory) ryz_host_display_save(directory,"v5-apps-stream-root-240");
    assert(ryz_v5_release() == ESP_OK);
    passed("latest V5 stream: 135px clipped viewport, partial fifth row, static 12px fonts and fixed header/TAB");
}

static void stream_navigation(void)
{
    catalog();
    const ryz_apps_snapshot_t browser = state.reader;
    assert(!has_back_asset(render_full()));
    down(70,70,100); up(120);
    assert(calls==1 && actions[0].action==RYZ_V5_APPS_SELECT);
    detail("app0.lua",9); (void)ryz_v5_apps_tick(130);
    assert(!strcmp(ryz_v5_apps_title(),"APP INFO") && has_back_asset(render_full()));
    down(20,16,140); up(150);
    assert(calls==2 && actions[1].action==RYZ_V5_APPS_BACK);
    assert(actions[1].offset==0 && actions[1].limit==0 && actions[1].index==0);
    assert(!strcmp(ryz_v5_apps_title(),"APPS"));
    (void)ryz_v5_apps_tick(160); /* A stale detail cannot undo Back. */
    assert(!strcmp(ryz_v5_apps_title(),"APPS") && calls==2);
    state.reader=browser; ++state.token.reader_request_id;
    state.reader.request_id=state.reader.completed_id=state.token.reader_request_id;
    (void)ryz_v5_apps_tick(170);
    assert(!has_back_asset(render_full()) && has_label(lv_screen_active(),"app0.lua"));
    passed("direct list select / detail / Back waits for real catalog acknowledgement");
}

static void back_tolerates_in_box_move(void)
{
    catalog();
    down(70, 70, 100); up(120);
    detail("app0.lua", 9); (void)ryz_v5_apps_tick(130);
    down(20, 16, 140);
    move(33, 16, 145); /* 13px drift, still inside the 44x32 back hit box. */
    up(150);
    assert(calls == 2 && actions[1].action == RYZ_V5_APPS_BACK);
    assert(actions[1].offset == 0 && actions[1].limit == 0 && actions[1].index == 0);

    catalog();
    down(70, 70, 200); up(220);
    detail("app0.lua", 9); (void)ryz_v5_apps_tick(230);
    down(20, 16, 240);
    move(45, 16, 245); /* Leaving the hit box must still cancel the press. */
    up(250);
    assert(calls == 1);
    passed("APP INFO back tolerates in-box touch drift but cancels out-of-box movement");
}

static void back_survives_reader_refresh(void)
{
    catalog();
    down(70, 70, 100); up(120);
    detail("app0.lua", 9); (void)ryz_v5_apps_tick(130);
    down(20, 16, 140);
    ++state.token.reader_request_id;
    state.reader.request_id = state.reader.completed_id = state.token.reader_request_id;
    (void)ryz_v5_apps_tick(145);
    up(150);
    assert(calls == 2 && actions[1].action == RYZ_V5_APPS_BACK);
    assert(actions[1].expected.reader_request_id == state.token.reader_request_id);
    passed("APP INFO back remains page-bound across an asynchronous metadata refresh");
}

static void window_fixture(size_t offset, size_t total, uint32_t revision, bool inserted)
{
    ++state.token.reader_request_id;
    state.reader = (ryz_apps_snapshot_t){.request_id=state.token.reader_request_id,
        .completed_id=state.token.reader_request_id,.view=RYZ_APPS_VIEW_CATALOG,
        .state=RYZ_APPS_READY,.store_status_valid=true,.store_ready=true};
    state.page_limit=RYZ_SCRIPT_STORE_PAGE_MAX;
    state.reader.page.offset=offset; state.reader.page.total=total;
    state.reader.page.revision=revision;
    state.reader.page.count=total-offset<RYZ_SCRIPT_STORE_PAGE_MAX?total-offset:RYZ_SCRIPT_STORE_PAGE_MAX;
    for(size_t i=0;i<state.reader.page.count;++i) {
        size_t absolute=offset+i;
        if(inserted && absolute==0) strcpy(state.reader.page.entries[i].name,"aaa.lua");
        else snprintf(state.reader.page.entries[i].name,sizeof(state.reader.page.entries[i].name),
                      "app%03u.lua",(unsigned)(absolute-(inserted?1U:0U)));
        state.reader.page.entries[i].bytes=1024;
    }
}

static void continuous_windows(void)
{
    catalog(); window_fixture(0,40,9,false); (void)ryz_v5_apps_tick(10);
    ryz_v5_apps_scroll(420);
    assert(calls==1 && actions[0].action==RYZ_V5_APPS_PAGE && actions[0].offset==10 &&
           actions[0].limit==RYZ_SCRIPT_STORE_PAGE_MAX);
    up(20); down(70,80,30); up(40);
    assert(calls==1); /* Loading never selects an old window. */
    window_fixture(10,40,9,false); (void)ryz_v5_apps_tick(50);
    lv_obj_t *root=render();
    (void)label_at(root,"app014.lua",42,47,106,20);
    down(70,65,60); up(70);
    assert(calls==2 && actions[1].action==RYZ_V5_APPS_SELECT && actions[1].index==4);
    detail("app014.lua",9); (void)ryz_v5_apps_tick(80);
    down(20,16,90); up(100);
    assert(calls==3 && actions[2].action==RYZ_V5_APPS_BACK);
    window_fixture(10,40,9,false); (void)ryz_v5_apps_tick(110);
    root=render(); (void)label_at(root,"app014.lua",42,47,106,20);
    ryz_v5_apps_reset(); ++state.token.page.generation;
    (void)ryz_v5_apps_tick(120); up(121);
    root=render(); (void)label_at(root,"app014.lua",42,47,106,20);
    window_fixture(10,41,10,true); (void)ryz_v5_apps_tick(130); up(131);
    root=render(); (void)label_at(root,"app014.lua",42,47,106,20);
    down(70,65,140); up(150);
    assert(calls==4 && actions[3].action==RYZ_V5_APPS_SELECT && actions[3].index==5);
    ryz_v5_apps_scroll(100000); up(160);
    assert(calls==5 && actions[4].action==RYZ_V5_APPS_PAGE && actions[4].offset==25 && actions[4].limit==16);
    window_fixture(25,41,10,true); (void)ryz_v5_apps_tick(170);
    root=render(); assert(has_label(root,"app039.lua"));
    down(70,180,180); move(70,80,190); move(70,160,200); up(210);
    assert(calls==5); /* The bottom stays vertical; reverse drag remains a drag. */
    assert(ryz_v5_release()==ESP_OK);
    passed("bounded 16-row windows preserve pixels/name across detail, route reentry and directory insertion");
}

static void detail_cancel_and_scroll(void)
{
    for(unsigned mode=0;mode<7;++mode) {
        catalog(); detail("app0.lua",9); (void)ryz_v5_apps_tick(1);
        down(120,218,100); (void)ryz_v5_apps_tick(1000);
        if(mode==0) { move(129,225,1100); move(120,218,1200); }
        else if(mode==1) { move(79,218,1100); move(120,218,1200); }
        else if(mode==2) { ryz_v5_apps_cancel_gesture(); }
        else if(mode==3) { (void)ryz_v5_apps_pointer(NULL,1100); }
        else if(mode==4) { state.reader.detail.file.sha256[0]='b'; (void)ryz_v5_apps_tick(1100); }
        else if(mode==5) { ++state.reader.detail.revision; (void)ryz_v5_apps_tick(1100); }
        else { ++state.token.page.generation; (void)ryz_v5_apps_tick(1100); }
        (void)ryz_v5_apps_tick(5000); up(5010);
        assert(calls==0);
    }
    catalog(); detail("app0.lua",9); (void)ryz_v5_apps_tick(1);
    memset(state.reader.detail.metadata.description,'W',RYZ_SCRIPT_METADATA_DESCRIPTION_MAX);
    state.reader.detail.metadata.description[RYZ_SCRIPT_METADATA_DESCRIPTION_MAX]=0;
    (void)ryz_v5_apps_tick(2); (void)render();
    down(120,110,100); move(120,60,110); up(120);
    lv_obj_t *root=render();
    assert(has_label(root,"HOLD 2S TO RUN"));
    (void)label_at(root,"HOLD 2S TO RUN",80,208,152,20);
    ryz_v5_apps_scroll(10000); root=render(); up(130);
    lv_obj_t *created=find_label_at(root,"CREATED",12,162,62,14);
    assert(created && calls==0);
    (void)label_at(root,"HOLD 2S TO RUN",80,208,152,20);
    down(120,218,200);
    ryz_touch_sample_t release={.event=RYZ_TOUCH_UP,.has_position=true,.x=80,.y=210};
    (void)ryz_v5_apps_pointer(&release,210);
    assert(calls==0); /* Final-position displacement cannot become a tap. */
    assert(ryz_v5_release()==ESP_OK);
    passed("detail hold cancels permanently for movement/loss/version/page; long metadata scrolls above fixed actions");
}

static void removed_anchor_boundary(void)
{
    catalog(); window_fixture(0,40,9,false); (void)ryz_v5_apps_tick(10);
    ryz_v5_apps_scroll(16*29); up(11);
    assert(calls==1 && actions[0].offset==12);
    window_fixture(12,40,9,false); (void)ryz_v5_apps_tick(20);
    window_fixture(16,39,10,false);
    for(size_t i=0;i<state.reader.page.count;++i)
        snprintf(state.reader.page.entries[i].name,sizeof(state.reader.page.entries[i].name),
                 "app%03u.lua",(unsigned)(17+i));
    (void)ryz_v5_apps_tick(30);
    assert(calls==2 && actions[1].offset==0 && actions[1].limit==16);
    window_fixture(0,39,10,false); (void)ryz_v5_apps_tick(40);
    assert(calls==3 && actions[2].offset==16 && actions[2].limit==16);
    window_fixture(16,39,10,false);
    for(size_t i=0;i<state.reader.page.count;++i)
        snprintf(state.reader.page.entries[i].name,sizeof(state.reader.page.entries[i].name),
                 "app%03u.lua",(unsigned)(17+i));
    (void)ryz_v5_apps_tick(50); up(51);
    for(unsigned i=0;i<20;++i) (void)ryz_v5_apps_tick(60+i);
    assert(calls==3); /* Missing anchor at the page seam must not ping-pong. */
    down(70,70,100); up(110);
    assert(calls==4 && actions[3].action==RYZ_V5_APPS_SELECT && actions[3].index==0);
    passed("deleted anchor exactly between reader windows selects the nearest successor without repeated reads");
}

static void stream_guards(void)
{
    stream_fixture();
    down(70,70,100); (void)ryz_v5_apps_tick(2000); up(2001);
    assert(calls==0);
    down(70,70,3000); move(90,70,3010); move(70,70,3020);
    (void)ryz_v5_apps_tick(4500); up(4501);
    assert(calls==0);
    down(70,55,5000); (void)ryz_v5_apps_tick(6500); up(6501);
    assert(calls==0); /* Separator is not a row. */
    ++state.reader.page.entries[0].bytes; ++state.token.reader_request_id;
    assert(ryz_v5_apps_tick(7000)); /* Actual list now displays reader updates. */
    ryz_v5_apps_reset(); ++state.token.page.generation;
    (void)ryz_v5_apps_tick(8000);
    down(70,70,8100); (void)ryz_v5_apps_tick(9500); up(9501);
    assert(calls==0); /* Reentry requires physical release. */
    down(70,70,9600); up(9610);
    assert(calls==1 && actions[0].action==RYZ_V5_APPS_SELECT);
    assert(ryz_v5_release()==ESP_OK);
    passed("initial release / movement / separator / reentry cannot accidentally run or delete");
}

static void browser_back_ignores_late_detail(void)
{
    catalog();
    const ryz_touch_sample_t brand={.pressed=true,.has_position=true,
        .x=20,.y=16,.event=RYZ_TOUCH_DOWN};
    assert(!ryz_v5_apps_pointer(&brand,100)); /* Brand is not an action. */
    detail("app0.lua",9); (void)ryz_v5_apps_tick(110);
    up(120); (void)ryz_v5_apps_tick(130);
    assert(calls==0); /* Late detail cannot reinterpret the uncaptured contact. */
    passed("brand contact followed by a new detail never becomes a backend action");
}

static void pixels(void)
{
    assert(ryz_font_init() == ESP_OK);
    catalog();
    lv_obj_t *root = render();
    assert(has_label(root, "app0.lua") && has_label(root, "08 TOTAL"));
    assert(ryz_host_display_pixel(8, 60) == rgb565(0xff6a00));
    assert(ryz_host_display_pixel(20, 54) == rgb565(0x121212));
    assert(ryz_v5_apps_has_navigation());
    down(70, 70, 100); (void)ryz_v5_apps_tick(1600);
    root = render();
    assert(!has_label(root, "HOLD 50% - MOVE TO CANCEL"));
    up(1601);
    detail("app0.lua", 9); (void)ryz_v5_apps_tick(1610);
    root = render();
    assert(!ryz_v5_apps_has_navigation() && has_label(root, "/scripts/app0.lua"));
    assert(has_label(root, "V1.0.0") && has_label(root, "VALID"));
    assert(ryz_host_display_pixel(82, 203) == rgb565(0xff6a00));
    down(120,210,1700); (void)ryz_v5_apps_tick(3200);
    root = render();
    assert(has_label(root,"HOLD 1.5 / 2S"));
    assert(ryz_host_display_pixel(100,234)==rgb565(0xff6a00));
    assert(ryz_host_display_pixel(180,234)==rgb565(0xff6a00));
    assert(ryz_host_display_pixel(210,234)==rgb565(0));
    /* 75% orange fill spans the button background, not just a bottom rail. */
    assert(ryz_host_display_pixel(100,204)==rgb565(0xff6a00));
    assert(ryz_host_display_pixel(210,204)==rgb565(0));
    up(3201);
    strcpy(state.reader.detail.metadata.version, "0.0.0-placeholder");
    (void)ryz_v5_apps_tick(715);
    root = render();
    lv_obj_t *version = find_text(root,"V0.0");
    assert(version && lv_obj_get_width(version)==62 && lv_label_get_long_mode(version)==LV_LABEL_LONG_DOT);
    strcpy(state.reader.detail.metadata.description, "\xe4\xb8\xad\xe6\x96\x87");
    (void)ryz_v5_apps_tick(720);
    root = render();
    assert(has_label(root, "[NON-ASCII]"));
    down(30, 210, 730); up(740);
    root = render();
    assert(has_label(root, "DELETE SCRIPT?"));
    assert(ryz_host_display_pixel(12, 80) == rgb565(0xff453a));
    assert(ryz_host_display_pixel(124, 190) == rgb565(0xff453a));
    assert(ryz_lvgl_system_release() == ESP_OK);
    ryz_v5_widgets_release();
    /* The production bridge retains its lazy display/draw allocation for
     * boot lifetime. Repeated page/font release must not accumulate more. */
    size_t retained_bytes = ryz_host_heap_bytes(), retained_blocks = ryz_host_heap_blocks();
    for (unsigned i = 0; i < 3; ++i) {
        catalog();
        (void)render();
        assert(ryz_lvgl_system_release() == ESP_OK);
        ryz_v5_widgets_release();
        assert(ryz_host_heap_bytes() == retained_bytes && ryz_host_heap_blocks() == retained_blocks);
    }
    passed("real LVGL RGB565 list/hold/detail/delete geometry; metadata is not fabricated");
}

static lv_obj_t *date_value(lv_obj_t *root,int x)
{
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *label=lv_obj_get_child(root,(int32_t)i);
        lv_obj_t *nested=date_value(label,x);
        if(nested) return nested;
        if(lv_obj_check_type(label,&lv_label_class) && lv_obj_get_x(label)==x &&
           lv_obj_get_y(label)>=189 && lv_obj_get_y(label)<205) return label;
    }
    return NULL;
}

static void date_fits(lv_obj_t *root,int x,const char *expected)
{
    lv_obj_t *label=date_value(root,x);
    assert(label && !strcmp(lv_label_get_text(label),expected));
    lv_area_t bounds; lv_obj_get_coords(label,&bounds);
    assert(lv_obj_get_width(label)==62 && bounds.y2<192);
    assert(lv_label_get_long_mode(label)==LV_LABEL_LONG_DOT);
    const lv_font_t *font=lv_obj_get_style_text_font(label,0);
    assert(font == ryz_v5_font_get(RYZ_FONT_MONO_MEDIUM,12));
    lv_point_t extent;
    lv_text_get_size(&extent,expected,font,lv_obj_get_style_text_letter_space(label,0),
        lv_obj_get_style_text_line_space(label,0),LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    assert(extent.x<=62 && extent.y<=lv_obj_get_height(label));
    unsigned ink=0;
    for(unsigned y=(unsigned)bounds.y1;y<=(unsigned)bounds.y2;++y) for(unsigned col=(unsigned)x;col<(unsigned)x+62;++col) {
        const uint16_t pixel=ryz_host_display_pixel(col,y);
        const unsigned red=(pixel>>11)&31U,green=(pixel>>5)&63U,blue=pixel&31U;
        /* White glyph coverage is brighter than the #181818 surface and
         * #3A3A3A borders; antialiasing need not produce a pure-white pixel. */
        if(red>7 && green>14 && blue>7) ++ink;
    }
    assert(ink>0);
}

static lv_obj_t *render_dates(int64_t created,int64_t modified,uint64_t tick,
                              const char *directory,const char *name)
{
    state.reader.detail.file.times.created_unix=created;
    state.reader.detail.file.times.modified_unix=modified;
    (void)ryz_v5_apps_tick(tick);
    const ryz_v5_apps_snapshot_t before=state;
    const ryz_system_ui_snapshot_t system={.time_hhmm="18:43"};
    const ryz_v5_status_t views={0};
    (void)ryz_v5_status_set(&views);
    const size_t shows=ryz_host_display_shows();
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,NULL,NULL,NULL)==ESP_OK);
    ryz_v5_apps_scroll(1000);
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_APPS,&system,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_host_display_shows()>shows && ryz_v5_widgets_status()==ESP_OK);
    assert(!memcmp(&state,&before,sizeof(state)) && calls==0);
    lv_obj_t *root=lv_screen_active();
    assert(has_label(root,"APP INFO") && has_label(root,"demo_times.lua"));
    assert(has_label(root,"DEMO INPUT") && has_label(root,"CREATED") && has_label(root,"MODIFIED"));
    if(directory && name) ryz_host_display_save(directory,name);
    return root;
}

static void dates(const char *directory)
{
    assert(ryz_font_init()==ESP_OK);
    catalog(); detail("demo_times.lua",9);
    strcpy(state.reader.detail.metadata.description,"DEMO INPUT");
    const char *prior=getenv("TZ");
    char *saved=prior?strdup(prior):NULL;
    assert(!prior || saved);
    assert(setenv("TZ","HST10",1)==0); tzset();
    const struct {
        int64_t created,modified; const char *creation,*modification,*image;
    } rows[]={
        {INT64_C(1709164799),INT64_C(1709164800),"02-28","02-29","v5-app-times-leap-demo-240"},
        {INT64_C(1709164800),INT64_C(1709251200),"02-29","03-01",NULL},
        {INT64_C(1735689599),INT64_C(1735689600),"12-31","01-01","v5-app-times-year-demo-240"},
        {INT64_C(253370764800),INT64_C(253402300799),"01-01","12-31","v5-app-times-9999-demo-240"},
        {0,INT64_C(1709164800),"-","02-29","v5-app-times-partial-demo-240"},
        {INT64_C(1709164800),0,"02-29","-",NULL},
        {0,0,"-","-","v5-app-times-unknown-demo-240"},
        {-1,INT64_C(253402300800),"-","-",NULL},
        {INT64_C(1704067199),INT64_MAX,"-","-",NULL},
        {INT64_MIN,-1,"-","-",NULL},
        {INT64_C(1704067200),INT64_C(1704067200),"01-01","01-01",NULL},
    };
    for(size_t i=0;i<sizeof(rows)/sizeof(rows[0]);++i) {
        lv_obj_t *root=render_dates(rows[i].created,rows[i].modified,100+i,directory,rows[i].image);
        date_fits(root,12,rows[i].creation); date_fits(root,87,rows[i].modification);
    }
    /* The timestamps are UTC even if a future localtime user changes TZ. */
    assert(setenv("TZ","CST-8",1)==0); tzset();
    lv_obj_t *root=render_dates(INT64_C(1735689599),INT64_C(1735689600),200,NULL,NULL);
    date_fits(root,12,"12-31"); date_fits(root,87,"01-01");
    root=render_dates(0,0,201,NULL,NULL);
    date_fits(root,12,"-"); date_fits(root,87,"-");
    assert(!has_label(root,"12-31") && !has_label(root,"01-01"));
    if(saved) { assert(setenv("TZ",saved,1)==0); free(saved); }
    else assert(unsetenv("TZ")==0);
    tzset();
    assert(ryz_v5_release()==ESP_OK);
    passed("UTC dates preserve independent created/modified identity, calendar limits and original 62px mono7 cells");
}

static void design_screenshots(const char *directory)
{
    if(!directory) return;
    catalog(); (void)render_full();
    ryz_host_display_save(directory,"v5-apps-list-start-240");
    ryz_v5_apps_scroll(1000); up(10); (void)render_full();
    ryz_host_display_save(directory,"v5-apps-list-end-240");
    detail("demo_info.lua",9);
    strcpy(state.reader.detail.metadata.description,"DEMO: Touch to place; AI controls opponent.");
    state.reader.detail.file.times.created_unix=INT64_C(1788480000);
    state.reader.detail.file.times.modified_unix=INT64_C(1788566400);
    (void)ryz_v5_apps_tick(20); (void)render_full();
    ryz_host_display_save(directory,"v5-app-info-top-240");
    down(120,218,100); (void)ryz_v5_apps_tick(1960); (void)render_full();
    ryz_host_display_save(directory,"v5-app-info-hold-240");
    up(2000); ryz_v5_apps_scroll(1000); up(2010); (void)render_full();
    ryz_host_display_save(directory,"v5-app-info-bottom-240");
    assert(ryz_v5_release()==ESP_OK);
}

int main(int argc, char **argv)
{
    assert(argc>=1 && argc<=3);
    if(argc>=2 && !strcmp(argv[1],"hold-cost")) {
        hold_feedback_cost(argc==3?argv[2]:NULL);
        hold_feedback_invalidation();
        return 0;
    }
    if (argc>=2 && !strcmp(argv[1],"stream")) {
        stream_root(argc==3?argv[2]:NULL);
        stream_navigation(); back_tolerates_in_box_move(); back_survives_reader_refresh();
        stream_guards(); browser_back_ignores_late_detail();
        printf("V5_APPS_UI_PASS %u groups\n",cases);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "gestures")) {
        interrupted_contacts_cancel();
        release_gesture_cancels_row(); release_tap_markers_select_once();
        row_drag_stays_cancelled(); row_snapshot_change_cancels();
        row_feedback_waits_for_stable_contact();
        back_tolerates_in_box_move(); back_survives_reader_refresh(); click_and_hold();
        printf("V5_APPS_UI_PASS %u groups\n", cases);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "pixels")) { pixels(); return 0; }
    if (argc>=2 && !strcmp(argv[1],"dates")) {
        dates(argc==3?argv[2]:NULL);
        printf("V5_APPS_UI_PASS %u groups\n",cases);
        return 0;
    }
    assert(argc<=2);
    stream_root(argc==2?argv[1]:NULL);
    stream_navigation(); back_tolerates_in_box_move(); back_survives_reader_refresh();
    continuous_windows(); detail_cancel_and_scroll(); removed_anchor_boundary();
    stream_guards(); browser_back_ignores_late_detail();
    click_and_hold(); gesture_cancel(); stale_and_release(); deletion();
    interrupted_contacts_cancel();
    release_gesture_cancels_row(); release_tap_markers_select_once();
    row_drag_stays_cancelled(); row_snapshot_change_cancels();
    row_feedback_waits_for_stable_contact();
    errors_and_back(); pixels(); dates(argc==2?argv[1]:NULL);
    hold_feedback_cost(NULL);
    hold_feedback_invalidation();
    design_screenshots(argc==2?argv[1]:NULL);
    printf("V5_APPS_UI_PASS %u groups\n", cases);
    return 0;
}
