#include "ryz_v5_ble.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_assets.h"

#include <stdio.h>
#include <string.h>

#define MED RYZ_FONT_BODY_MEDIUM
#define SEMI RYZ_FONT_BODY_SEMIBOLD
#define DISPLAY RYZ_FONT_DISPLAY
#define MONO RYZ_FONT_MONO
#define MONO_MED RYZ_FONT_MONO_MEDIUM
#define MONO_SEMI RYZ_FONT_MONO_SEMIBOLD
#define LEFT LV_TEXT_ALIGN_LEFT
#define RIGHT LV_TEXT_ALIGN_RIGHT
#define CENTER LV_TEXT_ALIGN_CENTER
#define BOX(x,y,w,h,c) ryz_v5_box(root,x,y,w,h,c,0,0)
#define TEXT(x,y,w,h,t,f,p,c,a) ryz_v5_label(root,x,y,w,h,t,f,p,c,a)

static ryz_v5_ble_binding_t s_binding;
void ryz_v5_ble_bind_services(const ryz_v5_ble_binding_t *binding)
{ s_binding=binding?*binding:(ryz_v5_ble_binding_t){0}; }
esp_err_t ryz_v5_ble_submit(ryz_v5_ble_action_t action,uint32_t operation_id,uint32_t value)
{ return s_binding.submit?s_binding.submit(s_binding.context,action,operation_id,value):ESP_ERR_INVALID_STATE; }

static bool valid(const ryz_v5_ble_model_t *model)
{ return model && model->page >= RYZ_V5_BLE_PAIRED && model->page <= RYZ_V5_BLE_FORGET &&
    model->state >= RYZ_V5_BLE_STATE_LEGACY && model->state <= RYZ_V5_BLE_STATE_STOPPING; }
static bool radio(const ryz_v5_ble_model_t *model)
{ return !model->unavailable && model->enabled; }
static bool bond(const ryz_v5_ble_model_t *model)
{ return !model->unavailable && model->bonded; }
static bool closure(const ryz_v5_ble_model_t *m)
{
    if(m->unavailable || m->page==RYZ_V5_BLE_INFO) return false;
    if(!m->checking_state && (m->page==RYZ_V5_BLE_REPLACE || m->page==RYZ_V5_BLE_FORGET) &&
        (m->state==RYZ_V5_BLE_STATE_SAVED_WAITING || m->state==RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT ||
         m->state==RYZ_V5_BLE_STATE_SERVICE_FAILED)) return false;
    return m->state!=RYZ_V5_BLE_STATE_LEGACY || m->checking_state;
}
static bool closure_confirmed(const ryz_v5_ble_model_t *m)
{
    if (m->unavailable || m->checking_state) return false;
    switch(m->state) {
    case RYZ_V5_BLE_STATE_SAVED_WAITING: return radio(m) && bond(m) && !m->linked;
    case RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT:
    case RYZ_V5_BLE_STATE_SERVICE_FAILED:
        return radio(m) && bond(m) && m->linked && m->service_known && !m->service_ready;
    case RYZ_V5_BLE_STATE_PAIR_TIMEOUT:
    case RYZ_V5_BLE_STATE_PAIR_FAILED:
        return radio(m) && m->bond_known && !m->bonded && !m->pairing_active;
    case RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE:
        return radio(m) && m->bond_known && !m->bonded && m->old_bond_removed && m->pairing_active;
    case RYZ_V5_BLE_STATE_FORGET_FAILED: return radio(m) && bond(m) && m->operation_id;
    case RYZ_V5_BLE_STATE_FORGETTING: return m->operation_id != 0;
    case RYZ_V5_BLE_STATE_VERIFY_CODE:
        return radio(m) && m->operation_id && m->pairing_active && m->compare_pending &&
            m->compare_value<=999999 && m->remaining_valid && m->remaining_seconds>0;
    case RYZ_V5_BLE_STATE_COMMITTING:
    case RYZ_V5_BLE_STATE_STOPPING: return m->operation_id != 0;
    case RYZ_V5_BLE_STATE_LEGACY: return true;
    }
    return false;
}
static bool inside(int x, int y, int left, int top, int width, int height)
{ return x >= left && x < left + width && y >= top && y < top + height; }

static const char *safe_text(char *out, size_t capacity, const char *source, size_t bound)
{
    size_t length = 0;
    while (length < bound && source[length]) ++length;
    if (!length || length == bound || length >= capacity) return "--";
    for (size_t index = 0; index < length; ++index) {
        const unsigned char byte = (unsigned char)source[index];
        out[index] = byte >= 0x20 && byte <= 0x7e ? (char)byte : '?';
    }
    out[length] = 0;
    return out;
}

static const char *unavailable_error(const ryz_v5_ble_model_t *model)
{
    size_t length=0;
    while(length<sizeof(model->error_code) && model->error_code[length]) {
        const unsigned char byte=(unsigned char)model->error_code[length++];
        if(byte<0x21 || byte>0x7e) return NULL;
    }
    return length && length<sizeof(model->error_code)?model->error_code:NULL;
}

static bool unavailable_failed(const ryz_v5_ble_model_t *model)
{
    return model->failure==RYZ_V5_BLE_FAILURE_START || model->failure==RYZ_V5_BLE_FAILURE_AUTH ||
        model->failure==RYZ_V5_BLE_FAILURE_SAVE || unavailable_error(model)!=NULL;
}

static void clipped(lv_obj_t *root, int x, int y, int w, int h,
                     const char *text, ryz_font_face_t face, unsigned size, uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = TEXT(x,y,w,h,text,face,size,color,align);
    ryz_v5_long_mode(label,LV_LABEL_LONG_DOT);
}

static void body(lv_obj_t *root, int x, int y, int w, int h, const char *text, lv_text_align_t align)
{
    lv_obj_t *label = TEXT(x,y,w,h,text,MED,12,V5_MUTED,align);
    if (!label) return;
    ryz_v5_long_mode(label,LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label,LV_SIZE_CONTENT);
    lv_obj_update_layout(label);
    int height = lv_obj_get_height(label);
    if (height > h) { height = h; lv_obj_set_height(label,h); }
    lv_obj_set_y(label,y+(h-height)/2);
}

static void closure_body(lv_obj_t *root,int x,int y,int w,int h,const char *value,
                          unsigned size,unsigned line,uint32_t color,lv_text_align_t align)
{
    lv_obj_t *label=TEXT(x,y,w,h,value,MED,size,color,align);
    if(!label) return;
    const lv_font_t *font=lv_obj_get_style_text_font(label,0);
    int spacing=(int)line-(int)font->line_height;
    if(lv_obj_get_style_text_line_space(label,0)!=spacing)
        lv_obj_set_style_text_line_space(label,spacing,0);
    lv_point_t measured;
    lv_text_get_size(&measured,value,font,0,spacing,w,LV_TEXT_FLAG_NONE);
    int height=measured.y<h?measured.y:h;
    lv_obj_set_pos(label,x,y+(h-height)/2); lv_obj_set_size(label,w,height);
    ryz_v5_long_mode(label,LV_LABEL_LONG_WRAP);
}

static void pill(lv_obj_t *root, int x, int y, int width, const char *text, uint32_t color)
{
    lv_obj_t *box = BOX(x,y,width,16,V5_SELECTED);
    ryz_v5_radius(box,2);
    TEXT(x,y,width,16,text,MONO_SEMI,12,color,CENTER);
}

static void button(lv_obj_t *root, int x, int y, int width, const char *text,
                    bool enabled, uint32_t color)
{
    uint32_t background = enabled ? color : V5_SELECTED;
    uint32_t ink = !enabled ? V5_DISABLED : color == V5_SELECTED ? V5_BODY : V5_BLACK;
    lv_obj_t *box = ryz_v5_box(root,x,y,width,28,background,0,0);
    ryz_v5_radius(box,2);
    TEXT(x,y,width,28,text,SEMI,14,ink,CENTER);
}

static lv_obj_t *footer(lv_obj_t *root, const char *right, const char *left)
{
    BOX(0,216,240,24,V5_BLACK);
    BOX(0,216,240,1,V5_BORDER);
    lv_obj_t *action=left?TEXT(8,216,78,24,left,SEMI,12,V5_BODY,LEFT):NULL;
    clipped(root,left?90:8,216,left?142:224,24,right,MONO_MED,12,V5_MUTED,RIGHT);
    return action;
}

static void master(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    TEXT(8,36,170,20,"BLUETOOTH LOW ENERGY",MONO_SEMI,12,V5_MUTED,LEFT);
    if(model->unavailable) {
        const bool failed=unavailable_failed(model);
        /* A status marker has no OFF-switch track/knob and no hit action. */
        TEXT(196,37,36,18,failed?"ERR":"N/A",MONO_SEMI,12,failed?V5_RED:V5_MUTED,CENTER);
        return;
    }
    const bool on = radio(model);
    lv_obj_t *track = BOX(196,37,36,18,on?V5_ORANGE:V5_DISABLED);
    ryz_v5_radius(track,9);
    lv_obj_t *knob = BOX(on?216:198,39,14,14,on?V5_BLACK:V5_BODY);
    ryz_v5_radius(knob,7);
}

static void row(lv_obj_t *root, int y, const char *label, const char *value,
                 bool active, bool enabled)
{
    if (active) BOX(8,y,224,34,V5_SELECTED);
    BOX(8,y,active?3:1,34,active?V5_ORANGE:V5_BORDER);
    BOX(8,y+33,224,1,V5_BORDER);
    TEXT(16,y,140,34,label,active?SEMI:MED,strlen(label)>16?12:14,enabled?V5_BODY:V5_DISABLED,LEFT);
    clipped(root,158,y,66,34,value,MONO_SEMI,14,active?V5_ORANGE:V5_MUTED,RIGHT);
}

static void paired(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    master(root,model);
    BOX(8,60,224,48,V5_SELECTED);
    const bool connected = radio(model) && model->linked;
    BOX(8,60,3,48,connected?V5_GREEN:V5_BORDER);
    char peer[33], details[48];
    clipped(root,16,62,138,22,model->unavailable?"BLE UNAVAILABLE":
        bond(model)?safe_text(peer,sizeof(peer),model->peer_name,sizeof(model->peer_name)):"NO PAIRED DEVICE",
        SEMI,13,V5_WHITE,LEFT);
    if (connected && model->rssi_valid)
        snprintf(details,sizeof(details),"RSSI %d dBm / %s",(int)model->rssi_dbm,bond(model)?"BONDED":"NO BOND");
    else snprintf(details,sizeof(details),"%s",model->unavailable?"Service unavailable":
                  bond(model)?"BONDED / RSSI --":"RSSI -- / NO BOND");
    clipped(root,16,84,208,16,details,MONO,12,V5_MUTED,LEFT);
    pill(root,172,62,52,connected?"LINKED":model->unavailable?"--":bond(model)?"SAVED":"--",
         connected?V5_GREEN:V5_MUTED);
    row(root,112,"BLE INFO","OPEN",true,true);
    row(root,146,"PAIR MODE",radio(model)?"READY":"--",false,radio(model));
    row(root,180,"FORGET DEVICE",radio(model)&&bond(model)?"OPEN":"LOCKED",false,radio(model)&&bond(model));
    footer(root,model->unavailable?"SERVICE UNAVAILABLE":model->reconnect_known?
        model->reconnect_enabled?"AUTO RECONNECT ON":"AUTO RECONNECT OFF":"RECONNECT --",NULL);
}

static void off(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    master(root,model);
    const bool failed=model->unavailable && unavailable_failed(model);
    /* Figma's former B text is not a Bluetooth glyph. Reuse the current
     * 40px export of its actual Header-derived pairing icon without scaling. */
    ryz_v5_image(root,100,71,&ryz_v5_asset_ble_pair,V5_DISABLED);
    BOX(72,92,96,3,V5_DISABLED);
    TEXT(24,122,192,26,failed?model->failure==RYZ_V5_BLE_FAILURE_START?"BLE INIT FAILED":"BLE FAILED":
         model->unavailable?"BLE UNAVAILABLE":"BLE OFF",DISPLAY,18,failed?V5_RED:V5_WHITE,CENTER);
    if(failed) {
        const char *code=unavailable_error(model);
        TEXT(24,150,192,24,code?code:"ERROR CODE UNKNOWN",MONO_MED,12,V5_RED,CENTER);
    } else body(root,24,148,192,32,model->unavailable?"BLE service unavailable. No radio state is assumed.":
              "Paired-device reconnect and pairing controls are disabled.",CENTER);
    row(root,182,"DEPENDENT SETTINGS","LOCKED",false,false);
    footer(root,failed?"RESTART DEVICE":model->unavailable?"SERVICE UNAVAILABLE":"",NULL);
}

static void empty(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    master(root,model);
    pill(root,8,62,model->unavailable?112:64,model->unavailable?"UNAVAILABLE":"NO BOND",
         model->unavailable?V5_MUTED:V5_WARNING);
    TEXT(16,88,208,34,model->unavailable?"BLE UNAVAILABLE":"NO PAIRED DEVICE",DISPLAY,22,V5_WHITE,CENTER);
    char window[88], policy[32];
    if (radio(model) && model->pairing_window_seconds)
        snprintf(window,sizeof(window),"Pairing closes automatically after %u seconds.",(unsigned)model->pairing_window_seconds);
    else snprintf(window,sizeof(window),"%s",model->unavailable?"BLE service unavailable.":"Pairing window not set.");
    body(root,28,124,184,34,window,CENTER);
    button(root,28,172,184,"START PAIRING",radio(model),V5_ORANGE);
    if (!model->unavailable && model->pairing_window_seconds)
        snprintf(policy,sizeof(policy),"%u SECOND WINDOW",(unsigned)model->pairing_window_seconds);
    else snprintf(policy,sizeof(policy),"%s",model->unavailable?"SERVICE UNAVAILABLE":"WINDOW --");
    footer(root,policy,NULL);
}

static void closure_pairing(lv_obj_t *root,const ryz_v5_ble_model_t *model);
static void pairing(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    const bool active = radio(model) && model->pairing_active;
    if(active) { closure_pairing(root,model); return; }
    TEXT(8,38,100,18,active?"DISCOVERABLE":"NOT ACTIVE",MONO_SEMI,12,V5_MUTED,LEFT);
    pill(root,168,38,64,active?"PAIRING":"--",active?V5_ORANGE:V5_MUTED);
    for (int index=0;index<3;++index) {
        lv_obj_t *ring = ryz_v5_box(root,78+index*10,62+index*10,84-index*20,84-index*20,
                                    V5_BLACK,index==2&&active?V5_ORANGE:V5_BORDER,1);
        if (ring) {
            ryz_v5_radius(ring,42-index*10);
            if(lv_obj_get_style_bg_opa(ring,0)!=LV_OPA_TRANSP) lv_obj_set_style_bg_opa(ring,LV_OPA_TRANSP,0);
        }
    }
    ryz_v5_image(root,100,84,&ryz_v5_asset_ble_pair,active?V5_ORANGE:V5_DISABLED);
    char countdown[16]="--:--";
    if (active && model->remaining_valid)
        snprintf(countdown,sizeof(countdown),"%02u:%02u",(unsigned)model->remaining_seconds/60,
                 (unsigned)model->remaining_seconds%60);
    TEXT(52,150,136,34,countdown,MONO_MED,28,V5_WHITE,CENTER);
    TEXT(24,184,192,18,active?"Waiting for a nearby device...":"Pairing is not active.",MED,12,V5_MUTED,CENTER);
    char policy[32]="SERVICE UNAVAILABLE";
    if (!model->unavailable) {
        if (model->pairing_window_seconds)
            snprintf(policy,sizeof(policy),"AUTO CLOSE %uS",(unsigned)model->pairing_window_seconds);
        else snprintf(policy,sizeof(policy),"%s","WINDOW --");
    }
    footer(root,policy,active?"CANCEL":"BACK");
}

static void warning(lv_obj_t *root, const ryz_v5_ble_model_t *model, bool forgetting)
{
    const bool allowed = radio(model) && bond(model);
    const uint32_t accent = forgetting?V5_RED:V5_WARNING;
    pill(root,8,38,forgetting?136:148,model->unavailable?"SERVICE UNAVAILABLE":
         forgetting?"DESTRUCTIVE ACTION":"PAIRED DEVICE EXISTS",model->unavailable?V5_MUTED:accent);
    char peer[33];
    const char *name = bond(model)?safe_text(peer,sizeof(peer),model->peer_name,sizeof(model->peer_name)):"--";
    if (!forgetting) clipped(root,160,38,72,16,name,MONO_MED,12,V5_MUTED,RIGHT);
    BOX(8,62,224,forgetting?104:96,V5_SELECTED);
    BOX(8,62,3,forgetting?104:96,accent);
    char title[48], paragraph[168];
    if (forgetting && model->unavailable) snprintf(title,sizeof(title),"%s","FORGET DEVICE?");
    else if (forgetting) snprintf(title,sizeof(title),"FORGET %s?",name);
    else snprintf(title,sizeof(title),"%s","REPLACE BOND?");
    clipped(root,18,68,202,28,title,DISPLAY,forgetting?18:20,V5_WHITE,LEFT);
    if (model->unavailable) {
        snprintf(paragraph,sizeof(paragraph),"%s","BLE service unavailable. No saved bond is assumed.");
    } else if (forgetting) {
        snprintf(paragraph,sizeof(paragraph),"%s","The saved bond and reconnect key will be removed. You can pair again later.");
    } else if (model->pairing_window_seconds) {
        snprintf(paragraph,sizeof(paragraph),"Continuing forgets %s, then opens a new %u-second pairing window.",
                 name,(unsigned)model->pairing_window_seconds);
    } else {
        snprintf(paragraph,sizeof(paragraph),"Continuing forgets %s, then opens a new pairing window.",name);
    }
    body(root,18,98,202,forgetting?56:50,paragraph,LEFT);
    const int y=forgetting?178:176;
    button(root,8,y,92,"CANCEL",true,V5_SELECTED);
    button(root,104,y,128,forgetting?"FORGET":"CONTINUE",allowed,forgetting?V5_RED:V5_WARNING);
    footer(root,model->unavailable?"SERVICE UNAVAILABLE":forgetting?"REQUIRES NEW PAIR":"OLD BOND KEPT ON CANCEL",NULL);
}

static void success(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    const bool saved = bond(model);
    pill(root,8,38,saved?64:112,saved?"PAIRED":"UNCONFIRMED",saved?V5_GREEN:V5_MUTED);
    if (saved) ryz_v5_image(root,103,75,&ryz_v5_asset_ble_success,V5_GREEN);
    else ryz_v5_image(root,100,70,&ryz_v5_asset_ble_pair,V5_DISABLED);
    char peer[33];
    clipped(root,24,124,192,28,saved?safe_text(peer,sizeof(peer),model->peer_name,sizeof(model->peer_name)):
            "BLE UNAVAILABLE",DISPLAY,22,V5_WHITE,CENTER);
    const char *reconnect = !saved ? "No confirmed pairing result." : model->reconnect_known ? model->reconnect_enabled?
        "Bond saved / automatic reconnect enabled":"Bond saved / automatic reconnect disabled":"Bond saved / reconnect status unknown";
    body(root,24,152,192,32,reconnect,CENTER);
    button(root,40,184,160,"DONE",true,V5_ORANGE);
    footer(root,saved?"BOND SAVED":"SERVICE UNAVAILABLE",NULL);
}

static int info_fields(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    const bool unknown=model->unavailable || model->checking_state;
    const bool connected = !unknown && radio(model) && model->linked;
    char local[33], address[18], peer[33], role[17], rssi[24]="--";
    if (connected && model->rssi_valid) snprintf(rssi,sizeof(rssi),"%d dBm",(int)model->rssi_dbm);
    const char *labels[]={"LOCAL","MAC","PEER","RSSI","ROLE","BOND","SERVICE"};
    const char *values[]={
        model->unavailable?"--":safe_text(local,sizeof(local),model->local_name,sizeof(model->local_name)),
        model->unavailable?"--":safe_text(address,sizeof(address),model->local_address,sizeof(model->local_address)),
        !unknown && bond(model)?safe_text(peer,sizeof(peer),model->peer_name,sizeof(model->peer_name)):"--",
        rssi,model->unavailable?"--":safe_text(role,sizeof(role),model->role,sizeof(model->role)),
        unknown?"--":bond(model)?"SAVED":model->state!=RYZ_V5_BLE_STATE_LEGACY && !model->bond_known?"--":"NONE",
        unknown||!model->service_known?"--":model->service_ready?"READY":"NOT READY"};
    ryz_v5_info_field_t fields[7];
    for(unsigned i=0;i<7;++i) fields[i]=(ryz_v5_info_field_t){labels[i],values[i]};
    if(root) (void)ryz_v5_info_draw(root,fields,7,model->info_scroll);
    return ryz_v5_info_height(fields,7);
}

int ryz_v5_ble_info_height(const ryz_v5_ble_model_t *model)
{ return model?info_fields(NULL,model):0; }

static void info(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    TEXT(8,36,160,18,"LOCAL + PHONE / PC",MONO_SEMI,12,V5_MUTED,LEFT);
    const bool unknown=model->unavailable || model->checking_state;
    const bool connected = !unknown && radio(model) && model->linked;
    pill(root,176,37,56,connected?"LINKED":unknown?"--":"IDLE",connected?V5_GREEN:V5_MUTED);
    (void)info_fields(root,model);
    const bool enabled=radio(model)&&bond(model) &&
        model->state!=RYZ_V5_BLE_STATE_FORGETTING && !model->checking_state;
    BOX(8,194,224,42,enabled?V5_RED:V5_SELECTED);
    TEXT(8,194,224,42,"FORGET PAIRED DEVICE",SEMI,14,enabled?V5_BLACK:V5_DISABLED,CENTER);
}

static void closure_overview(lv_obj_t *root,const ryz_v5_ble_model_t *m)
{
    bool linked=m->state==RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT;
    master(root,m);
    BOX(8,60,224,48,V5_SELECTED);
    BOX(8,60,3,48,linked?V5_GREEN:V5_BORDER);
    char peer[33];
    clipped(root,16,62,138,22,safe_text(peer,sizeof(peer),m->peer_name,sizeof(m->peer_name)),
        SEMI,14,V5_WHITE,LEFT);
    TEXT(16,84,208,16,linked?"BONDED / SERVICE WAIT":"BONDED / RSSI --",MONO,12,V5_MUTED,LEFT);
    pill(root,172,62,52,linked?"LINKED":"SAVED",linked?V5_GREEN:V5_MUTED);
    row(root,112,"BLE INFO","OPEN",true,true);
    row(root,146,"PAIR NEW","OPEN",false,true);
    row(root,180,"FORGET DEVICE","OPEN",false,true);
    footer(root,linked?"OPEN CLIENT APP":"WAITING FOR CLIENT",NULL);
}

static void closure_pairing(lv_obj_t *root,const ryz_v5_ble_model_t *m)
{
    const bool replaced=m->state==RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE;
    TEXT(8,38,150,18,replaced?"OLD BOND REMOVED":"NEW DEVICE",MONO_SEMI,12,V5_ORANGE,LEFT);
    pill(root,168,38,64,"OPEN",V5_ORANGE);
    for(int i=0;i<3;++i) {
        lv_obj_t *ring=ryz_v5_box(root,92+i*6,62+i*6,56-i*12,56-i*12,
            V5_BLACK,i==2?V5_ORANGE:V5_BORDER,1);
        if(ring) {
            ryz_v5_radius(ring,28-i*6);
            if(lv_obj_get_style_bg_opa(ring,0)!=LV_OPA_TRANSP) lv_obj_set_style_bg_opa(ring,LV_OPA_TRANSP,0);
        }
    }
    ryz_v5_image(root,100,70,&ryz_v5_asset_ble_pair,V5_WHITE);
    char time[16]="--:--",local[33];
    if(m->remaining_valid) snprintf(time,sizeof(time),"%02u:%02u",(unsigned)m->remaining_seconds/60,
        (unsigned)m->remaining_seconds%60);
    TEXT(52,120,136,32,time,MONO_MED,28,V5_ORANGE,CENTER);
    clipped(root,16,154,208,22,safe_text(local,sizeof(local),m->local_name,sizeof(m->local_name)),
        SEMI,14,V5_WHITE,CENTER);
    closure_body(root,16,180,208,32,"Select this device in\nyour phone / PC app.",12,16,V5_MUTED,CENTER);
    lv_obj_t *cancel=footer(root,replaced?"NO BOND TO KEEP":"NEW PAIR ONLY","CANCEL");
    if(cancel && !lv_color_eq(lv_obj_get_style_text_color(cancel,0),lv_color_hex(V5_ORANGE)))
        lv_obj_set_style_text_color(cancel,lv_color_hex(V5_ORANGE),0);
}

static void closure_notice(lv_obj_t *root,const ryz_v5_ble_model_t *m)
{
    bool confirmed=closure_confirmed(m);
    bool busy=m->state==RYZ_V5_BLE_STATE_FORGETTING || m->checking_state;
    bool service=m->state==RYZ_V5_BLE_STATE_SERVICE_FAILED;
    bool forgetting=m->state==RYZ_V5_BLE_STATE_FORGET_FAILED;
    uint32_t color=confirmed?(service?V5_WARNING:V5_RED):V5_WARNING;
    const char *tag="CHECK REQUIRED",*title="CHECKING SAVED STATE",*right="--";
    const char *paragraph="BLE state not confirmed.\nWait for a status check.\nNo action is assumed.";
    const char *status="STATE UNKNOWN";
    char error[17],message[144];
    if(confirmed) {
        right=safe_text(error,sizeof(error),m->error_code,sizeof(m->error_code));
        if(m->state==RYZ_V5_BLE_STATE_PAIR_TIMEOUT) {
            tag="WINDOW CLOSED"; title="PAIRING TIMED OUT";
            right=m->remaining_valid && !m->remaining_seconds?"00:00":"--:--";
            paragraph="No new device paired.\nOpen your phone / PC\napp, then try again."; status="NO DEVICE SAVED";
        } else if(m->state==RYZ_V5_BLE_STATE_PAIR_FAILED) {
            tag="PAIR FAILED"; title="PAIRING FAILED"; status="NO DEVICE SAVED";
            paragraph=m->failure==RYZ_V5_BLE_FAILURE_AUTH?
                "Phone / PC rejected\npairing. Check the app\nand try again.":
                m->failure==RYZ_V5_BLE_FAILURE_START?
                "Pairing could not start.\nCheck Bluetooth and\ntry again.":
                m->failure==RYZ_V5_BLE_FAILURE_SAVE?
                "Could not save pairing.\nCheck the device and\ntry again.":
                "Pairing did not finish.\nCheck the phone / PC\napp and try again.";
        } else if(forgetting) {
            tag="FORGET FAILED"; title="COULD NOT FORGET"; status="BOND STILL SAVED";
            paragraph="Could not remove pairing.\nYour saved device is kept.\nRetry or return to BLE.";
        } else if(service) {
            tag=title="CLIENT NOT READY"; status="BOND KEPT";
            paragraph="Bluetooth is connected.\nOpen or restart the app.\nNo need to pair again.";
        } else if(busy) {
            tag="PLEASE WAIT"; title="FORGETTING DEVICE"; status="DO NOT POWER OFF";
            char peer[33];
            snprintf(message,sizeof(message),"Disconnecting %s",
                safe_text(peer,sizeof(peer),m->peer_name,sizeof(m->peer_name)));
        }
    }
    pill(root,8,38,busy?136:148,tag,color);
    if(!busy) clipped(root,160,38,72,16,right,MONO_MED,12,V5_BODY,RIGHT);
    BOX(8,62,224,110,V5_SELECTED); BOX(8,62,3,110,color);
    clipped(root,18,68,202,28,title,DISPLAY,20,color,LEFT);
    if(busy && confirmed) {
        /* Keep the three-line notice visible at 14 px even with a 32-byte
         * peer name. Only the displayed name is elided; the model/identity
         * remains untouched and the full name is available in BLE INFO. */
        clipped(root,18,105,202,18,message,MED,14,V5_BODY,LEFT);
        TEXT(18,123,202,18,"and removing its pairing.",MED,14,V5_BODY,LEFT);
        TEXT(18,141,202,18,"Please wait for the result.",MED,14,V5_BODY,LEFT);
    } else closure_body(root,18,96,202,72,paragraph,14,18,V5_BODY,LEFT);
    if(busy) button(root,8,178,224,confirmed?"REMOVING...":"CHECKING...",false,V5_SELECTED);
    else {
        button(root,8,176,92,"BACK",true,V5_SELECTED);
        button(root,104,176,128,service?"BLE INFO":"RETRY",
            service || (confirmed && m->retry_allowed),V5_ORANGE);
    }
    footer(root,status,NULL);
}

static void verify_code(lv_obj_t *root,const ryz_v5_ble_model_t *m)
{
    char code[8],time[16];
    snprintf(code,sizeof(code),"%06u",(unsigned)m->compare_value);
    snprintf(time,sizeof(time),"%02u:%02u",(unsigned)m->remaining_seconds/60,
        (unsigned)m->remaining_seconds%60);
    pill(root,8,38,148,"VERIFY CODE",V5_ORANGE);
    TEXT(160,38,72,16,time,MONO_MED,12,V5_BODY,RIGHT);
    BOX(8,62,224,110,V5_SELECTED); BOX(8,62,3,110,V5_ORANGE);
    TEXT(18,70,202,40,code,MONO_MED,28,V5_ORANGE,CENTER);
    closure_body(root,18,120,202,40,"Match this code on\nboth devices.",14,18,V5_BODY,CENTER);
    button(root,8,176,92,"REJECT",true,V5_SELECTED);
    button(root,104,176,128,"CONFIRM",true,V5_ORANGE);
    footer(root,"ONLY IF CODES MATCH",NULL);
}

static void busy_operation(lv_obj_t *root,const ryz_v5_ble_model_t *m)
{
    bool saving=m->state==RYZ_V5_BLE_STATE_COMMITTING;
    pill(root,8,38,148,"PLEASE WAIT",V5_ORANGE);
    BOX(8,62,224,110,V5_SELECTED); BOX(8,62,3,110,V5_ORANGE);
    TEXT(18,68,202,28,saving?"SAVING PAIRING":"STOPPING BLUETOOTH",DISPLAY,20,V5_ORANGE,LEFT);
    closure_body(root,18,105,202,54,saving?"Verifying saved keys.\nPlease wait for the\nconfirmed result.":
        "Closing the connection.\nPlease wait for the\nconfirmed result.",14,18,V5_BODY,LEFT);
    button(root,8,178,224,saving?"SAVING...":"STOPPING...",false,V5_SELECTED);
    footer(root,saving?"DO NOT POWER OFF":"PLEASE WAIT",NULL);
}

static void closure_draw(lv_obj_t *root,const ryz_v5_ble_model_t *m)
{
    if(m->operation_id && (m->state==RYZ_V5_BLE_STATE_COMMITTING ||
        m->state==RYZ_V5_BLE_STATE_STOPPING)) {busy_operation(root,m);return;}
    if(!closure_confirmed(m)) {closure_notice(root,m);return;}
    if(m->state==RYZ_V5_BLE_STATE_VERIFY_CODE) verify_code(root,m);
    else if(m->state==RYZ_V5_BLE_STATE_SAVED_WAITING || m->state==RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT)
        closure_overview(root,m);
    else if(m->state==RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE) closure_pairing(root,m);
    else closure_notice(root,m);
}

const char *ryz_v5_ble_title(const ryz_v5_ble_model_t *model)
{
    if (!valid(model)) return "BLE";
    if(closure(model)) {
        if(model->state==RYZ_V5_BLE_STATE_VERIFY_CODE || model->state==RYZ_V5_BLE_STATE_COMMITTING)
            return "PAIR";
        if(model->state==RYZ_V5_BLE_STATE_STOPPING) return "PAIR";
        if(model->checking_state || model->state==RYZ_V5_BLE_STATE_FORGET_FAILED ||
            model->state==RYZ_V5_BLE_STATE_FORGETTING) return "FORGET";
        if(model->state==RYZ_V5_BLE_STATE_PAIR_TIMEOUT || model->state==RYZ_V5_BLE_STATE_PAIR_FAILED ||
            model->state==RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE) return "PAIR";
        return "BLE";
    }
    static const char *titles[]={"BLE","BLE","BLE","PAIR","PAIR","PAIR","BLE INFO","BLE INFO"};
    return titles[model->page];
}

esp_err_t ryz_v5_ble_draw(lv_obj_t *root, const ryz_v5_ble_model_t *model)
{
    if (!root || !valid(model)) return ESP_ERR_INVALID_ARG;
    BOX(0,32,240,208,V5_BLACK);
    if(closure(model)) {closure_draw(root,model);return ryz_v5_widgets_status();}
    switch (model->page) {
    case RYZ_V5_BLE_PAIRED: paired(root,model); break;
    case RYZ_V5_BLE_OFF: off(root,model); break;
    case RYZ_V5_BLE_EMPTY: empty(root,model); break;
    case RYZ_V5_BLE_PAIRING: pairing(root,model); break;
    case RYZ_V5_BLE_REPLACE: warning(root,model,false); break;
    case RYZ_V5_BLE_SUCCESS: success(root,model); break;
    case RYZ_V5_BLE_INFO: info(root,model); break;
    case RYZ_V5_BLE_FORGET: warning(root,model,true); break;
    }
    return ryz_v5_widgets_status();
}

ryz_v5_ble_action_t ryz_v5_ble_hit(const ryz_v5_ble_model_t *model, int x, int y)
{
    if (!valid(model) || !inside(x,y,0,0,240,240)) return RYZ_V5_BLE_ACTION_NONE;
    if(!model->unavailable && (model->state==RYZ_V5_BLE_STATE_FORGETTING ||
        model->state==RYZ_V5_BLE_STATE_COMMITTING || model->state==RYZ_V5_BLE_STATE_STOPPING || model->checking_state))
        return RYZ_V5_BLE_ACTION_NONE;
    if(closure(model)) {
        if(model->state==RYZ_V5_BLE_STATE_VERIFY_CODE) {
            if(inside(x,y,0,0,44,32)) return RYZ_V5_BLE_ACTION_CANCEL_PAIR;
            if(!closure_confirmed(model)) return RYZ_V5_BLE_ACTION_NONE;
            if(inside(x,y,8,176,92,28)) return RYZ_V5_BLE_ACTION_REJECT_CODE;
            if(inside(x,y,104,176,128,28)) return RYZ_V5_BLE_ACTION_CONFIRM_CODE;
            return RYZ_V5_BLE_ACTION_NONE;
        }
        if(model->state==RYZ_V5_BLE_STATE_FORGETTING) return RYZ_V5_BLE_ACTION_NONE;
        bool confirmed=closure_confirmed(model);
        bool pairing_after=model->state==RYZ_V5_BLE_STATE_PAIRING_AFTER_REPLACE;
        if(inside(x,y,0,0,44,32)) return confirmed && pairing_after?
            RYZ_V5_BLE_ACTION_CANCEL_PAIR:RYZ_V5_BLE_ACTION_BACK;
        if(confirmed && (model->state==RYZ_V5_BLE_STATE_SAVED_WAITING ||
            model->state==RYZ_V5_BLE_STATE_LINKED_SERVICE_WAIT)) {
            if(inside(x,y,188,32,52,28)) return RYZ_V5_BLE_ACTION_DISABLE;
            if(inside(x,y,8,112,224,34)) return RYZ_V5_BLE_ACTION_INFO;
            if(inside(x,y,8,146,224,34)) return RYZ_V5_BLE_ACTION_REPLACE;
            if(inside(x,y,8,180,224,34)) return RYZ_V5_BLE_ACTION_FORGET;
        } else if(confirmed && pairing_after) {
            if(inside(x,y,0,216,90,24)) return RYZ_V5_BLE_ACTION_CANCEL_PAIR;
        } else {
            if(inside(x,y,8,176,92,28)) return RYZ_V5_BLE_ACTION_BACK;
            if(inside(x,y,104,176,128,28)) {
                if(model->state==RYZ_V5_BLE_STATE_SERVICE_FAILED) return RYZ_V5_BLE_ACTION_INFO;
                if(confirmed && model->retry_allowed) return model->state==RYZ_V5_BLE_STATE_FORGET_FAILED?
                    RYZ_V5_BLE_ACTION_RETRY_FORGET:RYZ_V5_BLE_ACTION_RETRY_PAIR;
            }
        }
        return RYZ_V5_BLE_ACTION_NONE;
    }
    if (inside(x,y,0,0,44,32)) return model->page==RYZ_V5_BLE_PAIRING &&
        radio(model) && model->pairing_active?RYZ_V5_BLE_ACTION_CANCEL_PAIR:RYZ_V5_BLE_ACTION_BACK;
    const bool active = radio(model);
    if ((model->page == RYZ_V5_BLE_PAIRED || model->page == RYZ_V5_BLE_OFF || model->page == RYZ_V5_BLE_EMPTY) &&
        inside(x,y,188,32,52,28)) return model->unavailable?RYZ_V5_BLE_ACTION_NONE:
            model->enabled?RYZ_V5_BLE_ACTION_DISABLE:RYZ_V5_BLE_ACTION_ENABLE;
    switch (model->page) {
    case RYZ_V5_BLE_PAIRED:
        if (inside(x,y,8,112,224,34)) return RYZ_V5_BLE_ACTION_INFO;
        if (inside(x,y,8,146,224,34) && active)
            return bond(model)?RYZ_V5_BLE_ACTION_REPLACE:RYZ_V5_BLE_ACTION_PAIR;
        if (inside(x,y,8,180,224,34) && active && bond(model)) return RYZ_V5_BLE_ACTION_FORGET;
        break;
    case RYZ_V5_BLE_EMPTY:
        /* Invisible 8px padding; confirmation rows keep their separate bounds. */
        if (inside(x,y,20,164,200,44) && active)
            return bond(model)?RYZ_V5_BLE_ACTION_REPLACE:RYZ_V5_BLE_ACTION_PAIR;
        break;
    case RYZ_V5_BLE_PAIRING:
        if (inside(x,y,0,216,90,24)) return active&&model->pairing_active?
            RYZ_V5_BLE_ACTION_CANCEL_PAIR:RYZ_V5_BLE_ACTION_BACK;
        break;
    case RYZ_V5_BLE_REPLACE:
    case RYZ_V5_BLE_FORGET: {
        const int top = model->page == RYZ_V5_BLE_FORGET ? 178 : 176;
        if (inside(x,y,8,top,92,28)) return RYZ_V5_BLE_ACTION_BACK;
        if (inside(x,y,104,top,128,28) && active && bond(model))
            return model->page == RYZ_V5_BLE_FORGET?RYZ_V5_BLE_ACTION_CONFIRM_FORGET:RYZ_V5_BLE_ACTION_CONFIRM_REPLACE;
        break;
    }
    case RYZ_V5_BLE_SUCCESS:
        if (inside(x,y,40,184,160,28)) return RYZ_V5_BLE_ACTION_DONE;
        break;
    case RYZ_V5_BLE_INFO:
        if (inside(x,y,8,194,224,42) && active && bond(model) && !model->checking_state)
            return RYZ_V5_BLE_ACTION_FORGET;
        break;
    case RYZ_V5_BLE_OFF: break;
    }
    return RYZ_V5_BLE_ACTION_NONE;
}
