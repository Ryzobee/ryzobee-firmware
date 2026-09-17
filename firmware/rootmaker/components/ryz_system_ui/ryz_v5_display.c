#include "ryz_v5_display.h"
#include "ryz_v5_widgets.h"
#include <stdio.h>
#include <string.h>

/* Figma 94:3287. Drafts never rotate the panel or touch mapping. */
static ryz_v5_display_binding_t binding;
static ryz_display_settings_snapshot_t snapshot;
static ryz_display_settings_value_t draft, baseline;
static bool loaded, available, waiting, saved, failed, discard, guard;
static bool preview_owned;
static esp_err_t preview_error;
static uint64_t expected_operation;
typedef enum { HIT_NONE, HIT_BACK, HIT_ROT0, HIT_ROT1, HIT_ROT2, HIT_ROT3,
    HIT_MINUS, HIT_PLUS, HIT_BRIGHTNESS, HIT_SAVE, HIT_KEEP, HIT_DISCARD } hit_t;
static struct { bool active, cancelled; hit_t hit; int x,y; } capture;

static bool equal(ryz_display_settings_value_t a,ryz_display_settings_value_t b)
{ return a.rotation==b.rotation && a.sleep_index==b.sleep_index && a.brightness==b.brightness; }
static bool valid(ryz_display_settings_value_t v)
{ return v.rotation<4 && v.sleep_index<5 && v.brightness>=10 && v.brightness<=100 && v.brightness%5==0; }
static bool busy(void)
{ return waiting || snapshot.phase==RYZ_DISPLAY_SETTINGS_LOADING || snapshot.phase==RYZ_DISPLAY_SETTINGS_SAVING || snapshot.phase==RYZ_DISPLAY_SETTINGS_APPLYING; }
static bool dirty(void) { return loaded && !equal(draft,baseline); }
static bool editable(void) { return available && loaded && !busy() && !snapshot.input_blocked; }
static bool preview_available(void) { return binding.preview && binding.end_preview; }
static void end_preview(void)
{
    if(preview_owned && binding.end_preview) binding.end_preview(binding.context);
    preview_owned=false;
    preview_error=ESP_OK;
}

void ryz_v5_display_cancel_gesture(void)
{ capture.active=false; guard=true; }
void ryz_v5_display_reset(void)
{
    end_preview();
    snapshot=(ryz_display_settings_snapshot_t){0};
    draft=baseline=(ryz_display_settings_value_t){.rotation=0,.sleep_index=4,.brightness=50};
    loaded=available=waiting=saved=failed=discard=false;
    expected_operation=0; memset(&capture,0,sizeof(capture)); guard=true;
}
void ryz_v5_display_bind(const ryz_v5_display_binding_t *value)
{ ryz_v5_display_reset(); binding=value?*value:(ryz_v5_display_binding_t){0}; }

bool ryz_v5_display_tick(void)
{
    ryz_display_settings_snapshot_t next={0};
    if(!binding.get || binding.get(binding.context,&next)!=ESP_OK || !next.revision ||
       !valid(next.confirmed) || !valid(next.requested)) {
        bool changed=available; available=false;
        if(capture.active) ryz_v5_display_cancel_gesture();
        return changed;
    }
    bool changed=!available || snapshot.revision!=next.revision;
    if(changed && capture.active) ryz_v5_display_cancel_gesture();
    available=true; snapshot=next;
    if(!loaded && next.phase!=RYZ_DISPLAY_SETTINGS_LOADING) {
        draft=baseline=next.confirmed; loaded=true; changed=true;
    }
    if(waiting && next.operation_id>=expected_operation &&
       next.phase!=RYZ_DISPLAY_SETTINGS_SAVING && next.phase!=RYZ_DISPLAY_SETTINGS_APPLYING) {
        waiting=false;
        saved=next.operation_id==expected_operation && next.phase==RYZ_DISPLAY_SETTINGS_SAVED &&
              next.ready && next.persisted && next.error==ESP_OK && next.preview_error==ESP_OK &&
              equal(next.confirmed,draft);
        failed=!saved;
        if(saved) {
            baseline=next.confirmed;
            /* The service committed and retired the preview. A later reset
             * must not restore the pre-SAVE brightness. */
            preview_owned=false;
            preview_error=ESP_OK;
        }
        changed=true;
    }
    return changed;
}

static bool inside(int x,int y,int left,int top,int width,int height)
{ return x>=left && y>=top && x<left+width && y<top+height; }
static hit_t hit(int x,int y)
{
    if(discard) {
        if(inside(x,y,16,140,208,36)) return HIT_KEEP;
        if(inside(x,y,16,184,208,36)) return HIT_DISCARD;
        return HIT_NONE;
    }
    if(inside(x,y,0,0,44,32)) return HIT_BACK;
    if(!editable()) return HIT_NONE;
    for(int i=0;i<4;++i) if(inside(x,y,16+53*i,60,49,32)) return (hit_t)(HIT_ROT0+i);
    if(inside(x,y,96,101,34,40)) return HIT_MINUS;
    if(inside(x,y,198,101,34,40)) return HIT_PLUS;
    if(inside(x,y,8,146,224,42)) return HIT_BRIGHTNESS;
    if(inside(x,y,8,192,224,44) && (dirty() || failed || snapshot.load_error!=ESP_OK ||
       snapshot.phase==RYZ_DISPLAY_SETTINGS_APPLY_FAILED || snapshot.phase==RYZ_DISPLAY_SETTINGS_UNKNOWN)) return HIT_SAVE;
    return HIT_NONE;
}
static bool brightness(int x,bool explicit_down)
{
    if(x<16) x=16;
    if(x>224) x=224;
    uint8_t value=(uint8_t)(10+((x-16)*18+104)/208*5);
    bool changed=draft.brightness!=value;
    /* A fresh contact is an explicit retry after an admission/Owner error.
     * Held MOVE samples at the same value must never retry every frame. */
    bool retry=explicit_down && (preview_error!=ESP_OK || snapshot.preview_error!=ESP_OK);
    draft.brightness=value;
    if(changed || retry) {
        saved=failed=false;
        preview_error=preview_available()
            ? binding.preview(binding.context,snapshot.revision,value) : ESP_ERR_INVALID_STATE;
        if(preview_error==ESP_OK) preview_owned=true;
    }
    return changed || retry;
}
bool ryz_v5_display_pointer(const ryz_touch_sample_t *sample,bool *back)
{
    if(back) *back=false;
    if(!sample) { ryz_v5_display_cancel_gesture(); return false; }
    if(guard) { if(!sample->pressed) guard=false; return false; }
    if(sample->pressed && !sample->has_position) { ryz_v5_display_cancel_gesture(); return false; }
    if(!sample->pressed && sample->event!=RYZ_TOUCH_UP) {
        capture.active=false; return false;
    }
    if(sample->pressed && sample->event==RYZ_TOUCH_DOWN && capture.active) {
        ryz_v5_display_cancel_gesture(); return false;
    }
    if(sample->pressed && !capture.active) {
        if(sample->event!=RYZ_TOUCH_DOWN) return false;
        capture.active=true; capture.cancelled=false;
        capture.x=sample->x; capture.y=sample->y; capture.hit=hit(sample->x,sample->y);
        return capture.hit==HIT_BRIGHTNESS ? brightness(sample->x,true) : false;
    }
    if(!capture.active) return false;
    int dx=(int)sample->x-capture.x,dy=(int)sample->y-capture.y;
    if(sample->pressed) {
        if(capture.hit==HIT_BRIGHTNESS && !capture.cancelled && editable()) {
            if(dy<=-10 || dy>=10 || !inside(sample->x,sample->y,8,146,224,42)) capture.cancelled=true;
            else return brightness(sample->x,false);
        } else if(dx*dx+dy*dy>=100 || hit(sample->x,sample->y)!=capture.hit) capture.cancelled=true;
        return false;
    }
    hit_t action=capture.hit;
    bool cancelled=capture.cancelled;
    capture.active=false;
    /* CST816 can report a swipe only on a positionless UP when MOVE was
     * missed. Its raw direction is not rotated, so use it only to veto an
     * ordinary button action. Slider previews keep their own horizontal
     * ownership and can never turn their release into a SAVE. */
    if(action!=HIT_BRIGHTNESS && sample->gesture>=0x01 && sample->gesture<=0x04)
        cancelled=true;
    /* A release without coordinates is normal. A positioned release must
     * still be inside the same control; never manufacture a final hit. */
    if(cancelled || (sample->has_position && action!=HIT_BRIGHTNESS &&
       (dx*dx+dy*dy>=100 || hit(sample->x,sample->y)!=action))) return false;
    if(action==HIT_BACK) {
        if(waiting || (available && (snapshot.phase==RYZ_DISPLAY_SETTINGS_SAVING ||
           snapshot.phase==RYZ_DISPLAY_SETTINGS_APPLYING))) return false;
        if(dirty()) discard=true;
        else if(back) *back=true;
        return true;
    }
    if(action==HIT_KEEP) { discard=false; return true; }
    if(action==HIT_DISCARD) { end_preview(); discard=false; draft=baseline; if(back) *back=true; return true; }
    if(!editable()) return false;
    if(action>=HIT_ROT0 && action<=HIT_ROT3) {
        uint8_t rotation=(uint8_t)(action-HIT_ROT0);
        if(draft.rotation==rotation && !saved && !failed) return false;
        draft.rotation=rotation;
    }
    else if(action==HIT_MINUS && draft.sleep_index>0) --draft.sleep_index;
    else if(action==HIT_PLUS && draft.sleep_index<4) ++draft.sleep_index;
    else if(action==HIT_SAVE) {
        esp_err_t error=binding.submit ? binding.submit(binding.context,snapshot.revision,draft) : ESP_ERR_INVALID_STATE;
        waiting=error==ESP_OK; saved=false; failed=error!=ESP_OK;
        if(waiting) expected_operation=snapshot.operation_id+1;
        return true;
    } else return false;
    saved=failed=false; return true;
}

static void label(lv_obj_t *root,int x,int y,int w,int h,const char *text,
                  ryz_font_face_t face,unsigned px,uint32_t color,lv_text_align_t align)
{ ryz_v5_label(root,x,y,w,h,text,face,px,color,align); }
static void box(lv_obj_t *root,int x,int y,int w,int h,uint32_t color,uint32_t border,int width)
{ ryz_v5_box(root,x,y,w,h,color,border,width); }
static void button(lv_obj_t *root,int x,int y,int w,int h,const char *text,bool selected,bool enabled)
{
    box(root,x,y,w,h,selected?V5_ORANGE:V5_SURFACE,V5_BORDER,selected?0:1);
    label(root,x,y,w,h,text,RYZ_FONT_BODY_SEMIBOLD,14,
          !enabled?V5_DISABLED:selected?V5_BLACK:V5_WHITE,LV_TEXT_ALIGN_CENTER);
}
esp_err_t ryz_v5_display_draw(lv_obj_t *root)
{
    if(!root) return ESP_ERR_INVALID_ARG;
    bool enabled=editable(); char value[24];
    box(root,8,36,224,60,V5_SURFACE,0,0);
    label(root,16,39,152,18,"ORIENTATION",RYZ_FONT_BODY,12,V5_ORANGE,LV_TEXT_ALIGN_LEFT);
    label(root,188,39,36,18,"LCD",RYZ_FONT_BODY,12,V5_MUTED,LV_TEXT_ALIGN_RIGHT);
    static const char *angles[]={"0\xc2\xb0","90\xc2\xb0","180\xc2\xb0","270\xc2\xb0"};
    for(int i=0;i<4;++i) button(root,16+53*i,60,49,32,angles[i],draft.rotation==i,enabled);
    box(root,8,100,224,42,V5_SURFACE,0,0);
    label(root,16,102,76,18,"SLEEP",RYZ_FONT_BODY,14,V5_WHITE,LV_TEXT_ALIGN_LEFT);
    label(root,16,122,76,16,"IDLE OFF",RYZ_FONT_BODY,12,V5_MUTED,LV_TEXT_ALIGN_LEFT);
    button(root,96,101,34,40,"-",false,enabled && draft.sleep_index>0);
    button(root,198,101,34,40,"+",false,enabled && draft.sleep_index<4);
    static const char *sleep[]={"15 SEC","30 SEC","1 MIN","5 MIN","NEVER"};
    box(root,132,101,64,40,V5_SURFACE,V5_BORDER,1);
    label(root,132,101,64,40,sleep[draft.sleep_index],RYZ_FONT_BODY,14,V5_ORANGE,LV_TEXT_ALIGN_CENTER);
    box(root,8,146,224,42,V5_SURFACE,0,0);
    bool preview_failed=preview_error!=ESP_OK || snapshot.preview_error!=ESP_OK;
    const char *brightness_title=!preview_available()?"PREVIEW UNAVAILABLE":
        preview_failed?"PREVIEW FAILED":"BRIGHTNESS";
    label(root,16,147,132,18,brightness_title,RYZ_FONT_BODY,12,
          !preview_available() || preview_failed?V5_ORANGE:V5_WHITE,LV_TEXT_ALIGN_LEFT);
    snprintf(value,sizeof(value),"%u%%",draft.brightness);
    label(root,168,147,56,18,value,RYZ_FONT_BODY,14,V5_ORANGE,LV_TEXT_ALIGN_RIGHT);
    box(root,16,175,208,4,V5_BORDER,0,0);
    int offset=(draft.brightness-10)*208/90;
    box(root,16,175,offset,4,V5_ORANGE,0,0);
    lv_obj_t *thumb=ryz_v5_box(root,6+offset,167,20,20,V5_ORANGE,0,0);
    ryz_v5_radius(thumb,LV_RADIUS_CIRCLE);
    bool retry=failed || snapshot.load_error!=ESP_OK || snapshot.phase==RYZ_DISPLAY_SETTINGS_UNKNOWN ||
               snapshot.phase==RYZ_DISPLAY_SETTINGS_APPLY_FAILED;
    const char *action=!available?"DISPLAY UNAVAILABLE":busy()?"SAVING DISPLAY...":
        saved && !preview_failed?"SAVED":retry?"SAVE FAILED / RETRY":"SAVE DISPLAY";
    button(root,8,192,224,44,action,enabled && (dirty() || retry),available && (dirty() || retry || saved));
    if(discard) {
        box(root,8,68,224,164,V5_BG,V5_ORANGE,1);
        label(root,16,80,208,24,"DISCARD CHANGES?",RYZ_FONT_BODY_SEMIBOLD,14,V5_ORANGE,LV_TEXT_ALIGN_CENTER);
        label(root,16,108,208,20,"Saved display stays unchanged.",RYZ_FONT_BODY,12,V5_BODY,LV_TEXT_ALIGN_CENTER);
        button(root,16,140,208,36,"KEEP EDITING",false,true);
        button(root,16,184,208,36,"DISCARD",true,true);
    }
    return ryz_v5_widgets_status();
}
