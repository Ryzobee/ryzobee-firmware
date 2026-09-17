#include "ryz_v5_version.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_assets.h"
#include "ryz_v5_font.h"
#include <stdio.h>
#include <string.h>

#define L LV_TEXT_ALIGN_LEFT
#define C LV_TEXT_ALIGN_CENTER
#define R LV_TEXT_ALIGN_RIGHT
#define MONO RYZ_FONT_MONO
#define MS RYZ_FONT_MONO_SEMIBOLD
#define MM RYZ_FONT_MONO_MEDIUM
#define NS RYZ_FONT_BODY_SEMIBOLD
#define NM RYZ_FONT_BODY_MEDIUM
#define TK RYZ_FONT_DISPLAY
#define TXT(x,y,w,h,t,f,p,c,a) ryz_v5_label(root,x,y,w,h,t,f,p,c,a)
#define BOX(x,y,w,h,c) ryz_v5_box(root,x,y,w,h,c,0,0)

static const char *value(const char *text) { return text[0] ? text : "--"; }

static void kv(lv_obj_t *root,int y,const char *key,const char *text)
{
    TXT(8,y,64,24,key,RYZ_FONT_BODY,12,V5_MUTED,L);
    TXT(76,y,152,24,value(text),RYZ_FONT_BODY,14,V5_BODY,R);
}

static void display_version(char *out, size_t capacity, const char *raw)
{
    if (!out || !capacity) return;
    if (!raw || !raw[0]) {
        snprintf(out, capacity, "--");
        return;
    }
    if (raw[0] == 'V') snprintf(out, capacity, "%s", raw);
    else if (raw[0] == 'v') snprintf(out, capacity, "V%s", raw + 1);
    else if (raw[0] >= '0' && raw[0] <= '9') snprintf(out, capacity, "V%s", raw);
    else snprintf(out, capacity, "%s", raw);
}

static bool restart_enabled(const ryz_v5_version_model_t *m)
{ return m->image_staged && m->attempt_id && m->restart_ready && !m->restart_pending; }

static void card(lv_obj_t *root,int x,int y,int w,int h,uint32_t color,int radius)
{
    lv_obj_t *o=BOX(x,y,w,h,color);
    ryz_v5_radius(o,radius);
}

static void paragraph(lv_obj_t *root,int x,int y,int w,int h,const char *text,
                        uint32_t color,lv_text_align_t align)
{
    lv_obj_t *o=TXT(x,y,w,h,text,NM,12,color,align);
    if(!o) return;
    ryz_v5_long_mode(o,LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(o,x,y);
    lv_obj_set_size(o,w,h);
}

static const char *available_text(const ryz_v5_version_model_t *m)
{
    static const char safety[] =
        "Wi-Fi required · keep USB power connected · device restarts after confirmation.";
    return m && m->update_description[0] &&
        memchr(m->update_description,0,sizeof(m->update_description)) ?
        m->update_description : safety;
}

int ryz_v5_version_available_height(const ryz_v5_version_model_t *m)
{
    const lv_font_t *font=ryz_v5_font_get(NM,12);
    if(!font) return 48;
    lv_point_t size;
    lv_text_get_size(&size,available_text(m),font,0,0,192,LV_TEXT_FLAG_NONE);
    return size.y>48?size.y:48;
}

static void available_description(lv_obj_t *root,const ryz_v5_version_model_t *m)
{
    const int height=ryz_v5_version_available_height(m);
    const int maximum=height-48;
    int offset=m->info_scroll;
    if(offset<0) offset=0;
    if(offset>maximum) offset=maximum;
    lv_obj_t *viewport=BOX(24,118,192,48,V5_BLACK);
    if(!viewport) return;
    paragraph(viewport,0,-offset,192,height,available_text(m),V5_BODY,C);
    if(maximum) {
        int thumb=48*48/height;
        if(thumb<8) thumb=8;
        BOX(218,118,2,48,V5_BORDER);
        BOX(218,118+offset*(48-thumb)/maximum,2,thumb,V5_MUTED);
    }
}

static void pill(lv_obj_t *root,int x,int y,int w,const char *text,uint32_t color)
{
    card(root,x,y,w,16,V5_SELECTED,2);
    TXT(x,y,w,16,text,MS,12,color,C);
}

static void button(lv_obj_t *root,int x,int y,int w,const char *text,
                    uint32_t color,bool enabled)
{
    card(root,x,y,w,28,enabled?color:V5_SELECTED,2);
    TXT(x,y,w,28,text,NS,14,!enabled?V5_DISABLED:
        color==V5_SELECTED?V5_BODY:V5_BLACK,C);
}

static int info_fields(lv_obj_t *root,const ryz_v5_version_model_t *m)
{
    char running[sizeof(m->running)+2],idf[sizeof(m->idf)+2];
    display_version(running,sizeof(running),m->running);
    display_version(idf,sizeof(idf),m->idf);
    const ryz_v5_info_field_t fields[]={
        {"VERSION",running},{"BUILD",value(m->build)},{"IDF",idf},
        {"SLOT",value(m->slot)},{"UPTIME",value(m->uptime)},
        {NULL,value(m->chip)},
    };
    if(root) (void)ryz_v5_info_draw(root,fields,6,m->info_scroll);
    return ryz_v5_info_height(fields,6);
}

int ryz_v5_version_info_height(const ryz_v5_version_model_t *m)
{ return m?info_fields(NULL,m):0; }

static void footer(lv_obj_t *root,const char *text)
{
    BOX(0,216,240,24,V5_BLACK);
    BOX(0,216,240,1,V5_BORDER);
    TXT(8,216,224,24,text,MM,12,V5_MUTED,R);
}

const char *ryz_v5_version_title(ryz_v5_version_page_t page)
{ return page<=RYZ_V5_VERSION_INFO?"VERSION":"UPDATE"; }

esp_err_t ryz_v5_version_draw(lv_obj_t *root,const ryz_v5_version_model_t *m)
{
    if(!root || !m || m->page>RYZ_V5_VERSION_FAILED) return ESP_ERR_INVALID_ARG;
    BOX(0,32,240,208,V5_BLACK);
    char text[160];
    char running_version[sizeof(m->running) + 2U];
    char candidate_version[sizeof(m->candidate) + 2U];
    display_version(running_version, sizeof(running_version), m->running);
    display_version(candidate_version, sizeof(candidate_version), m->candidate);
    switch(m->page) {
    case RYZ_V5_VERSION_CURRENT:
        pill(root,8,38,80,m->checking?"CHECKING":m->checked&&m->up_to_date?
            "UP TO DATE":"NOT CHECKED",m->checked&&m->up_to_date?V5_GREEN:V5_MUTED);
        TXT(24,62,192,62,running_version,TK,42,V5_WHITE,C);
        kv(root,132,"UPDATED",m->updated);
        kv(root,156,"CHANNEL",m->channel);
        button(root,28,184,184,"CHECK FOR UPDATES",V5_ORANGE,m->check_ready);
        snprintf(text,sizeof(text),m->checked?"LATEST %s":"UPDATE SOURCE --",
            candidate_version);
        footer(root,text);
        break;
    case RYZ_V5_VERSION_INFO:
        TXT(8,36,110,18,"SYSTEM IMAGE",MS,12,V5_ORANGE,L);
        pill(root,176,37,56,m->healthy_known?(m->healthy?"HEALTHY":"PENDING"):"--",
            m->healthy_known&&m->healthy?V5_GREEN:V5_MUTED);
        (void)info_fields(root,m);
        card(root,8,194,224,42,V5_SELECTED,0);
        if(m->show_device_id && m->device_id[0]) {
            /* Keep the complete ID accessible without a tiny two-line font. */
            lv_obj_t *id=TXT(12,194,216,42,m->device_id,RYZ_FONT_BODY,14,V5_BODY,L);
            if(id) lv_label_set_long_mode(id,LV_LABEL_LONG_SCROLL_CIRCULAR);
        } else TXT(8,194,224,42,"SHOW DEVICE ID",NS,14,m->device_id[0]?V5_BODY:V5_DISABLED,C);
        break;
    case RYZ_V5_VERSION_AVAILABLE:
        pill(root,8,38,120,"UPDATE AVAILABLE",V5_WARNING);
        TXT(24,60,192,54,candidate_version,TK,38,V5_WARNING,C);
        available_description(root,m);
        pill(root,16,168,64,m->wifi_ready?"WIFI OK":"WIFI --",
            m->wifi_ready?V5_GREEN:V5_MUTED);
        pill(root,88,168,72,!m->power_known?"POWER --":m->power_ready?"POWER OK":"POWER LOW",
            m->power_known&&m->power_ready?V5_GREEN:V5_MUTED);
        pill(root,168,168,56,m->ab_slots?"A/B OTA":"SLOT --",m->ab_slots?V5_ORANGE:V5_MUTED);
        button(root,8,188,76,"LATER",V5_SELECTED,true);
        button(root,88,188,144,"UPDATE NOW",V5_WARNING,m->update_ready);
        footer(root,"UI SAMPLE VERSION");
        break;
    case RYZ_V5_VERSION_DOWNLOADING: {
        pill(root,8,38,96,m->cancelling?"CANCELLING":m->verifying?"VERIFYING":"DOWNLOADING",V5_ORANGE);
        unsigned percent=!m->total_bytes?0:m->downloaded_bytes>=m->total_bytes?100:
            (unsigned)((uint64_t)m->downloaded_bytes*100/m->total_bytes);
        if(m->total_bytes) snprintf(text,sizeof(text),"%u%%",percent);
        else snprintf(text,sizeof(text),"--%%");
        TXT(52,68,136,56,text,MM,42,V5_ORANGE,C);
        card(root,24,134,192,8,V5_SELECTED,4);
        card(root,24,134,(int)(192*percent/100),8,V5_ORANGE,4);
        if(m->total_bytes) snprintf(text,sizeof(text),"%lu / %lu B\n%s",
            (unsigned long)m->downloaded_bytes,(unsigned long)m->total_bytes,
            m->verifying?"Verifying downloaded image":"Verification follows download");
        else snprintf(text,sizeof(text),"%lu B / unknown\nVerification follows download",
            (unsigned long)m->downloaded_bytes);
        paragraph(root,24,152,192,34,text,V5_MUTED,C);
        button(root,40,188,160,"CANCEL DOWNLOAD",V5_SELECTED,m->cancel_ready);
        footer(root,"DO NOT POWER OFF");
        break;
    }
    case RYZ_V5_VERSION_REBOOT:
        pill(root,8,38,104,m->image_staged?"IMAGE VERIFIED":"IMAGE NOT READY",
            m->image_staged?V5_GREEN:V5_MUTED);
        if(m->image_staged) ryz_v5_image(root,105,71,&ryz_v5_asset_version_ready,V5_GREEN);
        TXT(24,116,192,30,m->image_staged?"READY TO REBOOT":"NOT READY",TK,22,V5_WHITE,C);
        paragraph(root,28,146,184,34,!m->image_staged?
            "No verified update is staged.\nReturn to Version and try again.":m->restart_pending?
            "Restart is in progress.\nPlease wait.":m->restart_error==ESP_ERR_TIMEOUT?
            "A task or file operation is busy.\nFinish it, then retry.":m->restart_error==ESP_FAIL?
            "Restart failed.\nStaged image unchanged.":m->restart_error!=ESP_OK?
            "Restart is blocked.\nCheck storage and retry.":
            "The new image is staged. Restart to switch A/B slot.",V5_BODY,C);
        button(root,8,188,76,"LATER",V5_SELECTED,true);
        button(root,88,188,144,"RESTART NOW",V5_ORANGE,restart_enabled(m));
        footer(root,!m->image_staged?"IMAGE NOT READY":m->restart_pending?"RESTART PENDING":
            m->restart_error==ESP_ERR_TIMEOUT?"RESTART BUSY":m->restart_error==ESP_FAIL?
            "RESTART FAILED":m->restart_error!=ESP_OK?"RESTART BLOCKED":
            m->ab_slots?"A/B IMAGE STAGED":"IMAGE STAGED");
        break;
    case RYZ_V5_VERSION_FAILED:
        pill(root,8,38,96,"UPDATE FAILED",V5_RED);
        snprintf(text,sizeof(text),"E_%04X",(unsigned)m->error);
        TXT(136,38,96,16,text,MS,12,V5_RED,R);
        card(root,8,62,224,96,V5_SURFACE,2);
        BOX(8,62,3,96,V5_RED);
        TXT(18,68,202,28,m->failure_title[0]?m->failure_title:"UPDATE UNAVAILABLE",TK,20,V5_WHITE,L);
        paragraph(root,18,98,202,50,m->failure_body[0]?m->failure_body:
            "No update source is configured. No download was started.",V5_BODY,L);
        button(root,8,178,76,"LATER",V5_SELECTED,true);
        button(root,88,178,144,"RETRY",V5_RED,m->retry_ready);
        footer(root,m->cleanup_pending?"CLEANUP PENDING":"CURRENT IMAGE RUNNING");
        break;
    }
    return ryz_v5_widgets_status();
}

static bool inside(uint16_t x,uint16_t y,int left,int top,int w,int h)
{ return x>=left && x<left+w && y>=top && y<top+h; }

ryz_v5_version_action_t ryz_v5_version_hit(const ryz_v5_version_model_t *m,
                                           uint16_t x,uint16_t y)
{
    if(!m || x>=240 || y>=240) return RYZ_V5_VERSION_ACTION_NONE;
    switch(m->page) {
    case RYZ_V5_VERSION_CURRENT:
        if(inside(x,y,24,62,192,62)) return RYZ_V5_VERSION_ACTION_INFO;
        if(m->check_ready && inside(x,y,28,184,184,28)) return RYZ_V5_VERSION_ACTION_CHECK;
        break;
    case RYZ_V5_VERSION_INFO:
        if(m->device_id[0] && inside(x,y,8,194,224,42)) return RYZ_V5_VERSION_ACTION_DEVICE_ID;
        break;
    case RYZ_V5_VERSION_AVAILABLE:
    case RYZ_V5_VERSION_REBOOT:
        if(inside(x,y,8,188,76,28)) return RYZ_V5_VERSION_ACTION_LATER;
        if(inside(x,y,88,188,144,28)) {
            if(m->page==RYZ_V5_VERSION_AVAILABLE && m->update_ready) return RYZ_V5_VERSION_ACTION_UPDATE;
            if(m->page==RYZ_V5_VERSION_REBOOT && restart_enabled(m)) return RYZ_V5_VERSION_ACTION_RESTART;
        }
        break;
    case RYZ_V5_VERSION_DOWNLOADING:
        if(m->cancel_ready && inside(x,y,40,188,160,28)) return RYZ_V5_VERSION_ACTION_CANCEL;
        break;
    case RYZ_V5_VERSION_FAILED:
        if(inside(x,y,8,178,76,28)) return RYZ_V5_VERSION_ACTION_LATER;
        if(m->retry_ready && inside(x,y,88,178,144,28)) return RYZ_V5_VERSION_ACTION_RETRY;
        break;
    }
    return RYZ_V5_VERSION_ACTION_NONE;
}
