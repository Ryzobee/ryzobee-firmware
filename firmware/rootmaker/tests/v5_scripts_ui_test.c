/* Original V5 Scripts + real system UI pointer/navigation/status controller.
 * The submit binding and service snapshots are fixtures, not filesystem,
 * boot persistence, restart or filesystem durability evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ryz_v5_scripts.h"
#include "ryz_v5_widgets.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"
#include "ryz_system_ui.h"
#include "ryz_v5_status.h"

#ifdef NDEBUG
#error "Scripts UI checks require assertions"
#endif

static unsigned groups;
static void passed(const char *name) { ++groups; printf("PASS %s\n",name); }
static lv_obj_t *find_label(lv_obj_t *root,const char *value);

static ryz_v5_scripts_snapshot_t fixture(void)
{
    ryz_v5_scripts_snapshot_t s = {
        .generation = 17, .boot_known = true, .boot_bytes_known = true,
        .boot_name = "boot.lua", .boot_bytes = 6451, .boot_revision = 3,
        .store_ready = true, .catalog_state = RYZ_APPS_READY,
        .can_open_picker = true, .can_change_boot = true, .can_clear_boot = true,
        .selected_identity_valid = true, .selected_revision = 3,
        .selected_source_valid = true,
    };
    s.catalog.revision = 3; s.catalog.total = 12; s.catalog.count = 12;
    const char *names[] = {"weather.lua", "clock.lua", "2048.lua", "touch_lab.lua"};
    for (unsigned i = 0; i < 4; ++i) {
        snprintf(s.catalog.entries[i].name, sizeof(s.catalog.entries[i].name), "%s", names[i]);
        s.catalog.entries[i].bytes = 6451 + i;
    }
    for (unsigned i = 4; i < 12; ++i) {
        snprintf(s.catalog.entries[i].name, sizeof(s.catalog.entries[i].name), "tool_%02u.lua", i);
        s.catalog.entries[i].bytes = 1024 + i;
    }
    memset(s.boot_sha256, 'b', 64); s.boot_sha256[64] = 0;
    s.selected.entry = s.catalog.entries[0];
    memset(s.selected.sha256, 'a', 64); s.selected.sha256[64] = 0;
    return s;
}

static void hit(ryz_v5_scripts_snapshot_t *s, unsigned x, unsigned y,
                  ryz_v5_scripts_action_t action)
{
    ryz_v5_scripts_intent_t out;
    memset(&out, 0xa5, sizeof(out));
    assert(ryz_v5_scripts_hit(s, (uint16_t)x, (uint16_t)y, &out));
    assert(out.action == action);
    if (action != RYZ_V5_SCRIPTS_ACTION_NONE) assert(out.generation == s->generation);
}

static void admission(void)
{
    ryz_v5_scripts_snapshot_t s = fixture();
    ryz_v5_scripts_intent_t out;
    hit(&s, 100, 145, RYZ_V5_SCRIPTS_ACTION_PICKER);
    hit(&s, 100, 179, RYZ_V5_SCRIPTS_ACTION_DELETE_CONFIRM);
    s.can_change_boot = false; s.can_open_picker = false; s.can_clear_boot = false;
    hit(&s, 100, 145, RYZ_V5_SCRIPTS_ACTION_NONE);
    hit(&s, 100, 179, RYZ_V5_SCRIPTS_ACTION_NONE);
    s = fixture(); s.page = RYZ_V5_SCRIPTS_PICKER;
    assert(ryz_v5_scripts_hit(&s, 90, 106, &out));
    assert(out.action == RYZ_V5_SCRIPTS_ACTION_SELECT && out.index == 1 && out.revision == 3);
    assert(!strcmp(out.name, "clock.lua"));
    assert(ryz_v5_scripts_hit(&s, 120, 198, &out));
    assert(out.action == RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT && out.revision == 3);
    assert(!strcmp(out.name, "weather.lua") && !strcmp(out.sha256, s.selected.sha256));
    ++s.selected_revision;
    hit(&s, 120, 198, RYZ_V5_SCRIPTS_ACTION_NONE);
    --s.selected_revision;
    s.selected.sha256[0] = 'g';
    hit(&s, 120, 198, RYZ_V5_SCRIPTS_ACTION_NONE);
    s.selected.sha256[0] = 'a';
    s.result = RYZ_V5_SCRIPTS_UNKNOWN;
    hit(&s, 120, 198, RYZ_V5_SCRIPTS_ACTION_NONE);
    s.result = RYZ_V5_SCRIPTS_IDLE;
    assert(ryz_v5_scripts_scroll(&s, 1, &out) && out.offset == 0 && out.limit == 16 && out.scroll_y == 1);
    assert(out.generation == 17 && out.revision == 3);
    assert(!ryz_v5_scripts_scroll(&s, -1, &out) && out.action == RYZ_V5_SCRIPTS_ACTION_NONE);
    s.scroll_y = 237;
    assert(!ryz_v5_scripts_scroll(&s, 1, &out));
    assert(ryz_v5_scripts_scroll(&s, -1, &out) && out.offset == 0 && out.scroll_y == 236);
    s.scroll_y = 0; s.catalog.total = 24; s.catalog.count = 16;
    assert(ryz_v5_scripts_scroll(&s, 558, &out) && out.offset == 8 && out.limit == 16 && out.scroll_y == 558);
    s = fixture();
    s.page = RYZ_V5_SCRIPTS_DELETE_ALL;
    hit(&s, 160, 194, RYZ_V5_SCRIPTS_ACTION_DELETE_ALL);
    assert(ryz_v5_scripts_hit(&s, 160, 194, &out));
    assert(!strcmp(out.name,"boot.lua") && !strcmp(out.sha256,s.boot_sha256));
    s.boot_sha256[0] = 'g'; hit(&s, 160, 194, RYZ_V5_SCRIPTS_ACTION_NONE);
    s.boot_sha256[0] = 'b';
    s.result = RYZ_V5_SCRIPTS_PENDING;
    hit(&s, 160, 194, RYZ_V5_SCRIPTS_ACTION_NONE);
    hit(&s, 50, 194, RYZ_V5_SCRIPTS_ACTION_CANCEL);
    memset(&out, 0xa5, sizeof(out));
    assert(!ryz_v5_scripts_hit(NULL, 10, 10, &out));
    assert(!out.action && !out.generation && !out.name[0] && !out.sha256[0]);
    s.generation = 0;
    hit(&s, 20, 16, RYZ_V5_SCRIPTS_ACTION_NONE);
    assert(!ryz_v5_scripts_hit(&s, 240, 240, &out));
    passed("Scripts bounds / exact boot SHA / selected source revision / pixel scroll and bounded window");
}

static uint16_t rgb565(unsigned rgb)
{ return (uint16_t)(((rgb >> 19) << 11) | (((rgb >> 10) & 63) << 5) | ((rgb >> 3) & 31)); }

static size_t green_pixels(void)
{
    size_t count = 0;
    for (unsigned y = 72; y < 106; ++y)
        for (unsigned x = 104; x < 136; ++x)
            if (ryz_host_display_pixel(x, y) == rgb565(V5_GREEN)) ++count;
    return count;
}

static void render(const ryz_v5_scripts_snapshot_t *s, const char *directory, const char *name)
{
    lv_obj_t *root = NULL;
    assert(ryz_lvgl_system_create(&root) == ESP_OK && root);
    ryz_v5_widgets_begin();
    assert(ryz_v5_scripts_draw(root, s) == ESP_OK);
    bool cancelled = true;
    assert(ryz_lvgl_system_commit(root, NULL, NULL, &cancelled) == ESP_OK && !cancelled);
    assert(ryz_v5_widgets_status() == ESP_OK);
    if (directory) ryz_host_display_save(directory, name);
}

static void pixels(const char *directory)
{
    assert(ryz_font_init() == ESP_OK);
    ryz_v5_scripts_snapshot_t s = fixture();
    render(&s, directory, "v5-scripts-settings-content");
    assert(ryz_host_display_pixel(8, 70) == rgb565(V5_ORANGE));
    assert(ryz_host_display_pixel(230, 64) == rgb565(V5_SELECTED));
    s.can_change_boot = false;
    render(&s, NULL, NULL);
    assert(find_label(lv_screen_active(),"COPIES AS boot.lua"));
    s.boot_known = false;
    render(&s, NULL, NULL);
    assert(find_label(lv_screen_active(),"UNKNOWN") && !find_label(lv_screen_active(),"NONE"));
    s = fixture();
    s.page = RYZ_V5_SCRIPTS_PICKER;
    render(&s, directory, "v5-scripts-picker-content");
    assert(ryz_host_display_pixel(24, 70) == rgb565(V5_ORANGE));
    assert(ryz_host_display_pixel(8, 70) == rgb565(V5_ORANGE));
    assert(ryz_host_display_pixel(100, 196) == rgb565(V5_ORANGE));
    s.page = RYZ_V5_SCRIPTS_SAVED;
    s.can_restart = true;
    render(&s, NULL, NULL);
    assert(!green_pixels());
    hit(&s, 160, 198, RYZ_V5_SCRIPTS_ACTION_NONE);
    s.operation = RYZ_V5_SCRIPTS_OP_BOOT;
    s.result = RYZ_V5_SCRIPTS_COMMITTED;
    strcpy(s.result_name, "weather.lua");
    render(&s, directory, "v5-scripts-saved-content");
    assert(green_pixels() > 100);
    hit(&s, 160, 198, RYZ_V5_SCRIPTS_ACTION_RESTART);
    s.error = ESP_FAIL;
    render(&s, NULL, NULL);
    assert(!green_pixels());
    hit(&s, 160, 198, RYZ_V5_SCRIPTS_ACTION_NONE);
    s = fixture(); s.page = RYZ_V5_SCRIPTS_DELETE_ALL;
    render(&s, directory, "v5-scripts-delete-content");
    assert(ryz_host_display_pixel(8, 80) == rgb565(V5_RED));
    assert(ryz_host_display_pixel(160, 180) == rgb565(V5_RED));
    s.can_clear_boot = false;
    render(&s, NULL, NULL);
    assert(ryz_host_display_pixel(160, 180) == rgb565(V5_SELECTED));
    assert(ryz_lvgl_system_release() == ESP_OK);
    ryz_v5_widgets_release();
    size_t retained = ryz_host_heap_bytes();
    s = fixture();
    render(&s, NULL, NULL);
    assert(ryz_lvgl_system_release() == ESP_OK);
    ryz_v5_widgets_release();
    assert(ryz_host_heap_bytes() == retained);
    passed("real LVGL/static glyph four-page geometry / exact check asset / no uncommitted success");
}

static ryz_system_ui_snapshot_t system_view;
static ryz_v5_scripts_snapshot_t service_view;
static ryz_v5_scripts_intent_t submitted[32];
static unsigned submission_count;

static esp_err_t submit(void *context,const ryz_v5_scripts_intent_t *intent)
{
    assert(context==submitted && intent && submission_count<32);
    submitted[submission_count++]=*intent;
    return ESP_OK; /* Acceptance only, never an invented committed result. */
}

static void publish(void)
{
    ryz_v5_status_t state={.ble={.unavailable=true}};
    state.scripts=service_view;
    (void)ryz_v5_status_set(&state);
}

static esp_err_t sample(ryz_touch_event_t event,bool pressed,bool positioned,uint16_t x,uint16_t y)
{
    const ryz_touch_sample_t point={.event=event,.pressed=pressed,.has_position=positioned,.x=x,.y=y};
    ryz_system_ui_action_t action=RYZ_SYSTEM_UI_ACTION_NETWORK_OFF;
    esp_err_t error=ryz_system_ui_process_sample(&point,ESP_OK,&system_view,&action);
    assert(action==RYZ_SYSTEM_UI_ACTION_NONE);
    return error;
}

static void released(void) { assert(sample(RYZ_TOUCH_NONE,false,false,0,0)==ESP_OK); }
static void tap(uint16_t x,uint16_t y)
{
    assert(sample(RYZ_TOUCH_DOWN,true,true,x,y)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,true,x,y)==ESP_OK);
}

static void fresh(ryz_v5_scripts_page_t page)
{
    assert(ryz_system_ui_reset_checked()==ESP_OK);
    ryz_host_display_reset();
    service_view=fixture(); service_view.page=page;
    system_view=(ryz_system_ui_snapshot_t){.state=RYZ_SYSTEM_UI_NETWORK_OFF,
        .storage_ready=true,.storage_total_bytes=1024*1024,.time_hhmm="18:43"};
    publish();
    const ryz_v5_scripts_binding_t binding={.submit=submit,.context=submitted};
    ryz_v5_scripts_bind(&binding);
    assert(ryz_system_ui_navigate(ryz_system_ui_page_token(),
        (ryz_system_ui_route_t)(RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS+page))==ESP_OK);
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    released();
    submission_count=0;
    memset(submitted,0,sizeof(submitted));
}

static void ready_service_page(ryz_v5_scripts_page_t page)
{
    service_view.page=page; ++service_view.generation; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    released();
}

static void selection_and_browse(const char *directory)
{
    fresh(RYZ_V5_SCRIPTS_SETTINGS);
    service_view.can_change_boot=false; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    tap(100,145);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SCRIPTS_PICKER && !submission_count);
    ready_service_page(RYZ_V5_SCRIPTS_PICKER);
    if(directory) ryz_host_display_save(directory,"v5-scripts-browse-only-240");
    tap(120,198); assert(!submission_count); /* Browse does not grant write permission. */
    const unsigned ys[]={72,103,134,165};
    for(unsigned row=0;row<4;++row) {
        tap(90,(uint16_t)ys[row]);
        assert(submission_count==row+1);
        const ryz_v5_scripts_intent_t *intent=&submitted[row];
        assert(intent->action==RYZ_V5_SCRIPTS_ACTION_SELECT && intent->index==row);
        assert(intent->generation==service_view.generation && intent->revision==service_view.catalog.revision);
        assert(!strcmp(intent->name,service_view.catalog.entries[row].name) && !intent->sha256[0]);
    }
    submission_count=0;
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,72)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,true,90,103)==ESP_OK);
    assert(!submission_count); /* Same action class is not the same row/identity. */
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,86)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,90)==ESP_OK); /* Adjacent row, under scroll threshold. */
    assert(sample(RYZ_TOUCH_UP,false,true,90,90)==ESP_OK);
    assert(!submission_count);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,94,108)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(submission_count==1 && submitted[0].index==1 && !strcmp(submitted[0].name,"clock.lua"));
    passed("real pointer browse-only navigation, four complete row identities, row drag rejection and positionless UP");
}

static void acknowledge_scroll(unsigned index)
{
    assert(submitted[index].action==RYZ_V5_SCRIPTS_ACTION_PAGE);
    service_view.scroll_y=submitted[index].scroll_y;
    ++service_view.generation; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
}

static void paging_gestures(void)
{
    fresh(RYZ_V5_SCRIPTS_PICKER);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,160)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,130)==ESP_OK);
    assert(submission_count==1 && submitted[0].action==RYZ_V5_SCRIPTS_ACTION_PAGE);
    assert(submitted[0].offset==0 && submitted[0].limit==16 && submitted[0].scroll_y==30);
    acknowledge_scroll(0);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,98)==ESP_OK);
    assert(submission_count==2 && submitted[1].scroll_y==62);
    acknowledge_scroll(1);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,160)==ESP_OK);
    assert(submission_count==3 && submitted[2].action==RYZ_V5_SCRIPTS_ACTION_PAGE && submitted[2].scroll_y==0);
    acknowledge_scroll(2);
    assert(sample(RYZ_TOUCH_UP,false,true,90,160)==ESP_OK);
    assert(submission_count==3); /* Returning to origin scrolls back, never selects. */

    fresh(RYZ_V5_SCRIPTS_PICKER);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,123)==ESP_OK); /* Already at top. */
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(!submission_count);

    service_view.scroll_y=237; ++service_view.generation; publish();
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,160)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,140)==ESP_OK); /* Already at bottom. */
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(!submission_count);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,123)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(submission_count==1 && submitted[0].offset==0 && submitted[0].limit==16 && submitted[0].scroll_y==217);
    passed("continuous pixel scroll accepts its own publication; top/bottom drag never selects a row");
}

static void sparse_swipes(void)
{
    for(unsigned gesture=1;gesture<=4;++gesture) for(unsigned apply=0;apply<2;++apply) {
        fresh(RYZ_V5_SCRIPTS_PICKER);
        unsigned y=apply?215:72;
        assert(sample(RYZ_TOUCH_DOWN,true,true,90,y)==ESP_OK);
        const ryz_touch_sample_t up={.event=RYZ_TOUCH_UP,.gesture=gesture};
        ryz_system_ui_action_t action;
        assert(ryz_system_ui_process_sample(&up,ESP_OK,&system_view,&action)==ESP_OK);
        assert(action==RYZ_SYSTEM_UI_ACTION_NONE && !submission_count);
        tap(90,y);
        assert(submission_count==1 && submitted[0].action==(apply?
            RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT:RYZ_V5_SCRIPTS_ACTION_SELECT));
    }
    fresh(RYZ_V5_SCRIPTS_PICKER);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,160)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,155)==ESP_OK);
    assert(!submission_count);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,150)==ESP_OK);
    assert(submission_count==1 && submitted[0].scroll_y==10);
    acknowledge_scroll(0);
    assert(sample(RYZ_TOUCH_UP,false,true,90,140)==ESP_OK);
    assert(submission_count==2 && submitted[1].action==RYZ_V5_SCRIPTS_ACTION_PAGE && submitted[1].scroll_y==20);
    passed("Picker sparse direction UP vetoes SELECT/APPLY, small steps and final positioned UP retain scroll distance");
}

static void stale_and_unpresented(void)
{
    for(unsigned changed=0;changed<2;++changed) {
        fresh(RYZ_V5_SCRIPTS_PICKER);
        assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
        if(changed) ++service_view.catalog.revision; else ++service_view.generation;
        publish();
        assert(sample(RYZ_TOUCH_MOVE,true,true,90,82)==ESP_OK);
        assert(sample(RYZ_TOUCH_UP,false,true,90,103)==ESP_OK);
        assert(!submission_count);
        /* New cache, old pixels: neither tap nor PAGE may use unseen data. */
        tap(90,103); assert(!submission_count);
        assert(sample(RYZ_TOUCH_DOWN,true,true,90,160)==ESP_OK);
        assert(sample(RYZ_TOUCH_MOVE,true,true,90,130)==ESP_OK);
        assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
        assert(!submission_count);
        assert(ryz_system_ui_render(&system_view)==ESP_OK);
        tap(90,103);
        assert(submission_count==1 && submitted[0].generation==service_view.generation &&
               submitted[0].revision==service_view.catalog.revision);
    }
    fresh(RYZ_V5_SCRIPTS_PICKER);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    strcpy(service_view.catalog.entries[1].name,"replacement.lua"); publish();
    assert(sample(RYZ_TOUCH_UP,false,true,90,103)==ESP_OK);
    assert(!submission_count); /* Full name remains part of the captured intent. */
    passed("stale generation/revision and unpresented snapshots reject taps and swipes; names stay captured");
}

static void input_failure_and_duplicates(void)
{
    fresh(RYZ_V5_SCRIPTS_PICKER);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,72)==ESP_OK);
    ryz_touch_sample_t invalid; memset(&invalid,0xa5,sizeof(invalid));
    ryz_system_ui_action_t action=RYZ_SYSTEM_UI_ACTION_NETWORK_OFF;
    assert(ryz_system_ui_process_sample(&invalid,ESP_FAIL,&system_view,&action)==ESP_FAIL);
    assert(action==RYZ_SYSTEM_UI_ACTION_NONE && !submission_count);
    assert(ryz_system_ui_render(&system_view)==ESP_OK);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    assert(sample(RYZ_TOUCH_MOVE,true,true,90,130)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,false,0,0)==ESP_OK);
    assert(!submission_count);
    released(); tap(90,103); assert(submission_count==1);
    assert(sample(RYZ_TOUCH_UP,false,true,90,103)==ESP_OK);
    assert(submission_count==1);
    submission_count=0;
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,72)==ESP_OK);
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,true,90,103)==ESP_OK);
    assert(!submission_count);
    released();
    assert(sample(RYZ_TOUCH_DOWN,true,true,2,50)==ESP_OK); /* Unarmed contact also counts. */
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    assert(sample(RYZ_TOUCH_UP,false,true,90,103)==ESP_OK);
    assert(!submission_count);
    released();
    assert(sample(RYZ_TOUCH_DOWN,true,true,90,103)==ESP_OK);
    released();
    assert(sample(RYZ_TOUCH_UP,false,true,90,103)==ESP_OK);
    assert(!submission_count);
    tap(90,103); assert(submission_count==1);
    passed("physical read failure revokes input; held recovery, duplicate DOWN/UP and lost UP cannot select");
}

static void modal_confirmation(const char *directory)
{
    fresh(RYZ_V5_SCRIPTS_SETTINGS);
    tap(100,179);
    assert(ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL && !submission_count);
    tap(160,194); assert(!submission_count); /* Settings snapshot cannot authorize a modal. */
    service_view.page=RYZ_V5_SCRIPTS_DELETE_ALL; ++service_view.generation; publish();
    tap(160,194); assert(!submission_count); /* Matching route, but not yet presented. */
    assert(ryz_system_ui_render(&system_view)==ESP_OK); released();
    if(directory) ryz_host_display_save(directory,"v5-scripts-modal-confirm-240");
    tap(50,194);
    assert(!submission_count && ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS);
    ready_service_page(RYZ_V5_SCRIPTS_SETTINGS);
    tap(100,179); ready_service_page(RYZ_V5_SCRIPTS_DELETE_ALL);
    const uint64_t generation=service_view.generation;
    tap(160,194);
    assert(submission_count==1 && submitted[0].action==RYZ_V5_SCRIPTS_ACTION_DELETE_ALL);
    assert(submitted[0].generation==generation && submitted[0].revision==service_view.catalog.revision);
    assert(sample(RYZ_TOUCH_UP,false,true,160,194)==ESP_OK && submission_count==1);
    service_view.operation=RYZ_V5_SCRIPTS_OP_DELETE_ALL;
    service_view.result=RYZ_V5_SCRIPTS_PENDING; service_view.can_clear_boot=false;
    publish(); assert(ryz_system_ui_render(&system_view)==ESP_OK);
    tap(160,194); assert(submission_count==1);
    passed("modal navigation waits for its own presented snapshot; cancel never submits and one confirmation submits once");
}

static lv_obj_t *find_label(lv_obj_t *root,const char *value)
{
    if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),value)) return root;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *found=find_label(lv_obj_get_child(root,(int32_t)i),value);
        if(found) return found;
    }
    return NULL;
}

static void deletion_result_pages(const char *directory)
{
    assert(ryz_system_ui_reset_checked()==ESP_OK);
    const char *names[]={"failed","missing","unknown","recovery","complete"};
    const char *titles[]={"CLEAR FAILED","NO BOOT SCRIPT","CHECK REQUIRED","BOOT SCRIPT CLEARED","BOOT SCRIPT CLEARED"};
    for(unsigned kind=0;kind<5;++kind) {
        ryz_v5_scripts_snapshot_t s=fixture();
        s.page=RYZ_V5_SCRIPTS_CLEAR_BOOT; s.operation=RYZ_V5_SCRIPTS_OP_CLEAR_BOOT;
        s.can_clear_boot=false;
        s.result=kind==2?RYZ_V5_SCRIPTS_UNKNOWN:kind>=3?RYZ_V5_SCRIPTS_COMMITTED:RYZ_V5_SCRIPTS_FAILED;
        s.error=kind==1?ESP_ERR_NOT_FOUND:kind==4?ESP_OK:ESP_FAIL;
        s.recovery_required=kind==2 || kind==3;
        s.cleanup_error=kind==3?ESP_ERR_TIMEOUT:ESP_OK;
        char name[64]; snprintf(name,sizeof(name),"v5-scripts-clear-%s-content",names[kind]);
        render(&s,directory,name);
        assert(find_label(lv_screen_active(),titles[kind]));
        assert(!find_label(lv_screen_active(),"DELETE ALL SCRIPTS"));
        assert(ryz_host_display_pixel(160,180)==rgb565(V5_SELECTED));
        hit(&s,160,194,RYZ_V5_SCRIPTS_ACTION_NONE);
        if(kind==2) assert(find_label(lv_screen_active(),"Check storage state.\nDo not repeat this write.\nOther scripts are kept."));
        if(kind==3) assert(find_label(lv_screen_active(),"STORAGE RECOVERY REQUIRED"));
        if(kind==4) {
            lv_obj_t *body=find_label(lv_screen_active(),"No user boot.lua.\nOther scripts are kept.\nNo automatic restart.");
            assert(body && lv_obj_get_height(body)==60);
            assert(lv_obj_get_y(body)>=98 && lv_obj_get_y(body)<=100);
            assert(lv_obj_get_style_text_line_space(body,0)+lv_obj_get_style_text_font(body,0)->line_height==20);
        }
    }
    assert(ryz_lvgl_system_release()==ESP_OK); ryz_v5_widgets_release();
    passed("clear result fixtures distinguish failed/missing/unknown/cleanup/committed; other files never targeted");
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    const char *directory=argc==2?argv[1]:NULL;
    admission(); pixels(directory);
    selection_and_browse(directory); paging_gestures(); sparse_swipes(); stale_and_unpresented();
    input_failure_and_duplicates(); modal_confirmation(directory); deletion_result_pages(directory);
    ryz_v5_scripts_bind(NULL);
    printf("V5_SCRIPTS_UI_PASS %u groups; real UI controller with service/binding fixtures\n",groups);
    return 0;
}
