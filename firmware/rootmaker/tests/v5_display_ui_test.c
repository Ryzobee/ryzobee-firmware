#include "ryz_v5_display.h"
#include "ryz_v5_render.h"
#include "ryz_v5_widgets.h"
#include "ryz_display_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ryz_display_settings_snapshot_t state;
static ryz_display_settings_value_t request;
static unsigned requests,previews,preview_ends;
static uint64_t revision,preview_revision;
static uint8_t light,preview_request;
static esp_err_t get_error,submit_error,preview_admission_error;
static bool back;
static esp_err_t get(void *context,ryz_display_settings_snapshot_t *out)
{ assert(context==&state); *out=state; return get_error; }
static esp_err_t submit(void *context,uint64_t expected,ryz_display_settings_value_t value)
{ assert(context==&state); ++requests; request=value; revision=expected; return submit_error; }
static esp_err_t preview(void *context,uint64_t expected,uint8_t brightness)
{
    assert(context==&state); ++previews; preview_revision=expected; preview_request=brightness;
    if(preview_admission_error==ESP_OK) light=brightness;
    return preview_admission_error;
}
static void end_preview(void *context)
{ assert(context==&state); ++preview_ends; light=state.confirmed.brightness; }
static void pointer(ryz_touch_event_t event,int x,int y)
{
    ryz_touch_sample_t sample={.event=event,.x=x,.y=y,
        .pressed=event==RYZ_TOUCH_DOWN || event==RYZ_TOUCH_MOVE,
        .has_position=event==RYZ_TOUCH_DOWN || event==RYZ_TOUCH_MOVE};
    (void)ryz_v5_display_pointer(&sample,&back);
}
static void click(int x,int y) { pointer(RYZ_TOUCH_DOWN,x,y); pointer(RYZ_TOUCH_UP,x,y); }
static void fixture(void)
{
    ryz_v5_display_bind(NULL); /* Retire the previous fixture's binding first. */
    state=(ryz_display_settings_snapshot_t){.revision=10,.operation_id=3,.ready=true,
        .phase=RYZ_DISPLAY_SETTINGS_READY,.confirmed={0,4,50},.requested={0,4,50}};
    get_error=submit_error=preview_admission_error=ESP_OK;
    requests=previews=preview_ends=0; light=50; preview_request=0; preview_revision=0;
    const ryz_v5_display_binding_t binding={.get=get,.submit=submit,.context=&state,
        .preview=preview,.end_preview=end_preview};
    ryz_v5_display_bind(&binding); assert(ryz_v5_display_tick()); pointer(RYZ_TOUCH_UP,0,0);
}
static lv_obj_t *find(lv_obj_t *root,const char *text)
{
    if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return root;
    for(unsigned i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *result=find(lv_obj_get_child(root,i),text); if(result) return result;
    }
    return NULL;
}
static lv_obj_t *draw(const char *directory,const char *name)
{
    const ryz_system_ui_snapshot_t system={.firmware_version="DEMO",.time_hhmm="18:43"};
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_DISPLAY,&system,NULL,NULL,NULL)==ESP_OK);
    assert(ryz_v5_widgets_status()==ESP_OK);
    if(directory) ryz_host_display_save(directory,name);
    return lv_screen_active();
}
static void cancel_contract(void)
{
    fixture(); click(90,75);
    pointer(RYZ_TOUCH_MOVE,120,216); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    pointer(RYZ_TOUCH_DOWN,120,216); pointer(RYZ_TOUCH_NONE,120,216); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    pointer(RYZ_TOUCH_DOWN,120,216); pointer(RYZ_TOUCH_DOWN,120,216); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    pointer(RYZ_TOUCH_DOWN,120,216); pointer(RYZ_TOUCH_MOVE,127,224); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    pointer(RYZ_TOUCH_DOWN,120,216); ++state.revision; (void)ryz_v5_display_tick(); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    pointer(RYZ_TOUCH_DOWN,120,216); get_error=ESP_ERR_TIMEOUT; (void)ryz_v5_display_tick();
    get_error=ESP_OK; (void)ryz_v5_display_tick(); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    pointer(RYZ_TOUCH_DOWN,120,216); (void)ryz_v5_display_pointer(NULL,&back); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    pointer(RYZ_TOUCH_DOWN,120,216); ryz_v5_display_reset(); (void)ryz_v5_display_tick(); pointer(RYZ_TOUCH_UP,120,216); assert(requests==0);
    click(45,15); assert(!back); click(20,15); assert(back);
    fixture(); state.phase=RYZ_DISPLAY_SETTINGS_LOADING; state.ready=false; ++state.revision;
    (void)ryz_v5_display_tick(); click(20,15); assert(back);
    ryz_v5_display_bind(NULL); (void)ryz_v5_display_tick(); pointer(RYZ_TOUCH_UP,0,0);
    click(20,15); assert(back); /* Unavailable service cannot trap the user. */
}
static void gesture_release(unsigned gesture)
{
    const ryz_touch_sample_t sample={.event=RYZ_TOUCH_UP,.gesture=(uint8_t)gesture};
    (void)ryz_v5_display_pointer(&sample,&back);
}
static void sparse_swipes_cancel_actions(void)
{
    for(unsigned gesture=1;gesture<=4;++gesture) {
        fixture(); click(90,75);
        pointer(RYZ_TOUCH_DOWN,120,216); gesture_release(gesture);
        assert(requests==0); /* A sparse swipe is not SAVE. */
        gesture_release(gesture); assert(requests==0);
        click(120,216); assert(requests==1 && request.rotation==1);

        fixture();
        pointer(RYZ_TOUCH_DOWN,90,75); gesture_release(gesture);
        click(204,171); click(120,216);
        assert(requests==1 && request.rotation==0 && request.brightness==90);

        fixture();
        pointer(RYZ_TOUCH_DOWN,110,122); gesture_release(gesture);
        click(90,75); click(120,216);
        assert(requests==1 && request.sleep_index==4);
        fixture(); click(110,122);
        pointer(RYZ_TOUCH_DOWN,210,122); gesture_release(gesture);
        click(120,216); assert(requests==1 && request.sleep_index==3);

        fixture();
        pointer(RYZ_TOUCH_DOWN,20,15); gesture_release(gesture);
        assert(!back && requests==0);
        click(20,15); assert(back);

        fixture(); click(204,171); click(20,15);
        pointer(RYZ_TOUCH_DOWN,120,202); gesture_release(gesture);
        assert(!back && light==90 && preview_ends==0 && requests==0);
        assert(find(draw(NULL,NULL),"DISCARD CHANGES?"));
        click(120,202); assert(back && light==50 && preview_ends==1);

        fixture(); click(204,171); click(20,15);
        pointer(RYZ_TOUCH_DOWN,120,158); gesture_release(gesture);
        assert(!back && light==90 && preview_ends==0 && requests==0);
        assert(find(draw(NULL,NULL),"DISCARD CHANGES?"));
        click(120,158); assert(!find(draw(NULL,NULL),"DISCARD CHANGES?"));

        /* Slider ownership is intentionally different: horizontal MOVE
         * previews continuously, while its release never becomes SAVE. */
        fixture();
        pointer(RYZ_TOUCH_DOWN,16,171); pointer(RYZ_TOUCH_MOVE,224,171);
        assert(previews==2 && light==100 && requests==0);
        gesture_release(gesture);
        assert(previews==2 && light==100 && requests==0 && preview_ends==0);
        click(120,216); assert(requests==1 && request.brightness==100);

        fixture();
        pointer(RYZ_TOUCH_DOWN,16,171); pointer(RYZ_TOUCH_MOVE,224,171);
        pointer(RYZ_TOUCH_MOVE,120,216); gesture_release(gesture);
        assert(previews==2 && light==100 && requests==0 && preview_ends==0);
        click(120,216); assert(requests==1 && request.brightness==100);
    }
    assert(ryz_v5_release()==ESP_OK);
}
static void preview_contract(const char *directory)
{
    fixture();
    pointer(RYZ_TOUCH_DOWN,16,160);
    assert(previews==1 && preview_request==10 && light==10 && preview_revision==10 && requests==0);
    pointer(RYZ_TOUCH_MOVE,224,160);
    assert(previews==2 && preview_request==100 && light==100 && requests==0);
    pointer(RYZ_TOUCH_MOVE,224,160); assert(previews==2); /* Same value is not another intent. */
    assert(!ryz_v5_display_tick()); /* Normal previews do not revoke this contact. */
    pointer(RYZ_TOUCH_MOVE,204,160);
    assert(previews==3 && light==90 && preview_revision==10 && requests==0);
    pointer(RYZ_TOUCH_UP,204,160);
    click(204,160); assert(previews==3); /* Healthy same-value DOWN is also a no-op. */
    lv_obj_t *root=draw(directory,"v5-display-preview-240");
    assert(find(root,"90%") && find(root,"BRIGHTNESS") && !find(root,"SAVED"));
    assert(state.confirmed.brightness==50 && state.requested.brightness==50);
    click(20,15); assert(!back);
    click(120,158); assert(!back && light==90 && preview_ends==0 && requests==0); /* KEEP. */
    click(20,15); click(120,202);
    assert(back && light==50 && preview_ends==1 && requests==0); /* DISCARD. */
    ryz_v5_display_reset(); assert(preview_ends==1); /* Restore is not duplicated. */

    fixture(); click(204,171); assert(light==90 && previews==1);
    ryz_v5_display_reset(); assert(light==50 && preview_ends==1 && requests==0);
    (void)ryz_v5_display_tick(); pointer(RYZ_TOUCH_UP,0,0);
    root=draw(NULL,NULL); assert(find(root,"50%"));

    /* Rebinding must release through the OLD callback and context. */
    click(204,171); assert(light==90);
    const ryz_v5_display_binding_t replacement={0};
    ryz_v5_display_bind(&replacement);
    assert(light==50 && preview_ends==2 && requests==0);

    fixture(); click(204,171); click(120,216);
    assert(requests==1 && light==90 && preview_ends==0);
    state.requested=state.confirmed=request; state.operation_id=4;
    state.phase=RYZ_DISPLAY_SETTINGS_SAVED; state.persisted=true; ++state.revision;
    (void)ryz_v5_display_tick(); root=draw(NULL,NULL); assert(find(root,"SAVED"));
    click(20,15); assert(back);
    ryz_v5_display_reset(); assert(light==90 && preview_ends==0); /* Saved baseline is final. */

    fixture(); click(204,171); submit_error=ESP_ERR_TIMEOUT; click(120,216);
    assert(requests==1 && light==90 && preview_ends==0);
    root=draw(NULL,NULL); assert(find(root,"SAVE FAILED / RETRY") && !find(root,"SAVED"));
    click(20,15); click(120,158); assert(light==90 && preview_ends==0);
    click(20,15); click(120,202); assert(back && light==50 && preview_ends==1);

    fixture(); click(204,171); click(120,216);
    state.requested=request; state.operation_id=4; state.phase=RYZ_DISPLAY_SETTINGS_UNKNOWN;
    state.error=ESP_ERR_TIMEOUT; ++state.revision;
    (void)ryz_v5_display_tick(); root=draw(NULL,NULL);
    assert(find(root,"SAVE FAILED / RETRY") && light==90 && preview_ends==0);
    click(20,15); click(120,202); assert(back && light==50 && preview_ends==1);
}
static void preview_errors(const char *directory)
{
    fixture(); preview_admission_error=ESP_ERR_TIMEOUT; click(204,171);
    assert(previews==1 && light==50 && requests==0);
    lv_obj_t *root=draw(directory,"v5-display-preview-failed-240");
    assert(find(root,"PREVIEW FAILED") && !find(root,"SAVED"));
    ryz_v5_display_reset(); assert(preview_ends==0); /* Rejected request owns nothing. */

    fixture(); preview_admission_error=ESP_ERR_TIMEOUT;
    pointer(RYZ_TOUCH_DOWN,204,171); assert(previews==1 && light==50);
    preview_admission_error=ESP_OK;
    pointer(RYZ_TOUCH_MOVE,204,171); assert(previews==1 && light==50); /* No held retry. */
    pointer(RYZ_TOUCH_UP,204,171);
    pointer(RYZ_TOUCH_DOWN,204,171);
    assert(previews==2 && preview_request==90 && light==90 && requests==0);
    pointer(RYZ_TOUCH_MOVE,204,171); assert(previews==2);
    pointer(RYZ_TOUCH_UP,204,171);
    root=draw(NULL,NULL); assert(find(root,"BRIGHTNESS") && !find(root,"PREVIEW FAILED"));
    click(204,171); assert(previews==2); /* Retry success restores no-op semantics. */
    ryz_v5_display_reset(); assert(light==50 && preview_ends==1);

    fixture(); click(204,171); preview_admission_error=ESP_ERR_TIMEOUT; click(16,171);
    assert(previews==2 && light==90 && requests==0);
    root=draw(NULL,NULL); assert(find(root,"PREVIEW FAILED"));
    ryz_v5_display_reset(); assert(light==50 && preview_ends==1); /* Older accepted preview still owned. */

    fixture(); click(204,171); assert(light==90);
    state.preview_error=ESP_ERR_TIMEOUT; ++state.revision;
    (void)ryz_v5_display_tick(); root=draw(NULL,NULL);
    assert(find(root,"PREVIEW FAILED") && !find(root,"SAVED"));
    click(20,15); click(120,202); assert(back && light==50 && preview_ends==1);

    fixture(); click(204,171); assert(previews==1);
    state.preview_error=ESP_ERR_TIMEOUT; ++state.revision;
    (void)ryz_v5_display_tick();
    pointer(RYZ_TOUCH_DOWN,204,171);
    assert(previews==2 && preview_request==90 && preview_revision==state.revision && requests==0);
    pointer(RYZ_TOUCH_MOVE,204,171); pointer(RYZ_TOUCH_MOVE,204,171); assert(previews==2);
    pointer(RYZ_TOUCH_UP,204,171);
    state.preview_error=ESP_OK; ++state.revision; /* Owner completes the explicit retry. */
    (void)ryz_v5_display_tick(); root=draw(NULL,NULL);
    assert(find(root,"BRIGHTNESS") && !find(root,"PREVIEW FAILED"));
    click(204,171); assert(previews==2 && light==90);
    ryz_v5_display_reset(); assert(light==50 && preview_ends==1);

    fixture();
    const ryz_v5_display_binding_t unavailable={.get=get,.submit=submit,.context=&state};
    ryz_v5_display_bind(&unavailable); (void)ryz_v5_display_tick(); pointer(RYZ_TOUCH_UP,0,0);
    click(204,171); root=draw(directory,"v5-display-preview-unavailable-240");
    assert(find(root,"PREVIEW UNAVAILABLE") && !find(root,"SAVED"));
    assert(previews==0 && light==50 && requests==0);
    click(20,15); click(120,202); assert(back && preview_ends==0);
}
static void closure(const char *directory)
{
    fixture();
    lv_obj_t *root=draw(directory,"v5-display-default-240");
    assert(find(root,"NEVER") && find(root,"50%") && find(root,"0\xc2\xb0"));
    click(120,216); assert(requests==0); /* Unchanged saves are disabled. */
    click(90,75); click(110,122);
    pointer(RYZ_TOUCH_DOWN,30,160); pointer(RYZ_TOUCH_MOVE,204,171); pointer(RYZ_TOUCH_UP,204,171);
    /* Vertical movement cancels a brightness drag permanently. */
    click(204,171);
    root=draw(directory,"v5-display-draft-240");
    assert(find(root,"5 MIN") && find(root,"90%") && requests==0);
    click(20,15); assert(!back);
    root=draw(directory,"v5-display-discard-240"); assert(find(root,"DISCARD CHANGES?"));
    click(120,158); assert(!back);
    root=draw(NULL,NULL); assert(!find(root,"DISCARD CHANGES?"));
    click(120,216); assert(requests==1 && revision==10 && request.rotation==1 && request.sleep_index==3 && request.brightness==90);
    root=draw(directory,"v5-display-saving-240"); assert(find(root,"SAVING DISPLAY...") && !find(root,"SAVED"));
    click(120,216); click(20,15); assert(requests==1 && !back);
    state.requested=request; state.operation_id=4; state.phase=RYZ_DISPLAY_SETTINGS_SAVING; ++state.revision;
    (void)ryz_v5_display_tick();
    state.phase=RYZ_DISPLAY_SETTINGS_APPLYING; state.persisted=true; ++state.revision;
    (void)ryz_v5_display_tick(); root=draw(NULL,NULL); assert(!find(root,"SAVED"));
    state.phase=RYZ_DISPLAY_SETTINGS_SAVED; state.confirmed=request; ++state.revision;
    (void)ryz_v5_display_tick(); root=draw(directory,"v5-display-saved-240"); assert(find(root,"SAVED"));
    click(20,15); assert(back);
    /* Initial snapshots from somebody else's completed operation are not our Saved. */
    ryz_v5_display_reset(); (void)ryz_v5_display_tick(); pointer(RYZ_TOUCH_UP,0,0);
    root=draw(NULL,NULL); assert(!find(root,"SAVED"));
    click(150,75); click(120,216); assert(requests==2);
    state.requested=request; state.operation_id=5; state.phase=RYZ_DISPLAY_SETTINGS_UNKNOWN; state.persisted=false;
    state.error=ESP_ERR_TIMEOUT; ++state.revision;
    (void)ryz_v5_display_tick(); root=draw(directory,"v5-display-failed-240");
    assert(find(root,"SAVE FAILED / RETRY") && !find(root,"SAVED"));
    click(120,216); assert(requests==3 && request.rotation==2); /* Explicit retry retains exact draft. */
    state.operation_id=6; state.phase=RYZ_DISPLAY_SETTINGS_APPLY_FAILED; ++state.revision;
    (void)ryz_v5_display_tick(); click(20,15); click(120,202); assert(back);
    assert(ryz_v5_release()==ESP_OK);
}
int main(int argc,char **argv)
{
    assert(argc==1 || argc==2);
    const char *directory=argc==2?argv[1]:NULL;
    sparse_swipes_cancel_actions();
    cancel_contract(); preview_contract(directory); preview_errors(directory); closure(directory);
    puts("V5_DISPLAY_UI_PASS: live preview intents, discard/reset ownership, static glyphs, safe captures, real-result gated Saved/Retry");
    return 0;
}
