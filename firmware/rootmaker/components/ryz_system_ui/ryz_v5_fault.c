#include "ryz_v5_fault.h"
#include "ryz_v5_widgets.h"
#include "ryz_qr_encoder.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>

static ryz_v5_fault_model_t fault={.line=-1};
static char link_path[96];
static bool show_log;
static bool presented_link;
static bool tainted;
enum { QR_SIDE=123 };
typedef struct { lv_image_dsc_t image; uint8_t alpha[QR_SIDE*QR_SIDE]; } fault_qr_t;

static void wipe(void *data,size_t length)
{ volatile uint8_t *bytes=data; while(length--) *bytes++=0; }

void ryz_v5_fault_set(const ryz_v5_fault_model_t *model)
{
    fault=model?*model:(ryz_v5_fault_model_t){.line=-1};
    fault.code[sizeof(fault.code)-1]=0;
    fault.summary[sizeof(fault.summary)-1]=0;
    fault.source_file[sizeof(fault.source_file)-1]=0;
    wipe(link_path,sizeof(link_path));
    show_log=false;
    presented_link=false;
}

bool ryz_v5_fault_link(const char *path)
{
    if(!path || strncmp(path,"/f/",3) || strlen(path)>=sizeof(link_path)) path="";
    bool changed=strcmp(link_path,path)!=0;
    if(!changed) return false;
    wipe(link_path,sizeof(link_path));
    snprintf(link_path,sizeof(link_path),"%s",path);
    if(!link_path[0]) show_log=false;
    return changed;
}

static void qr_free(fault_qr_t *qr)
{
    lv_image_cache_drop(&qr->image);
    wipe(qr->alpha,sizeof(qr->alpha));
    heap_caps_free(qr);
}

static void qr_deleted(lv_event_t *event)
{ qr_free(lv_event_get_user_data(event)); }

bool ryz_v5_fault_needs_cleanup(void) { return tainted; }
esp_err_t ryz_v5_fault_cleanup(void)
{
    if(!tainted) return ESP_OK;
    esp_err_t error=ryz_display_privacy_clear();
    if(error==ESP_OK) tainted=false;
    return error;
}

static esp_err_t qr_matrix(void *context,int side,ryz_qr_module_reader_t module,const void *matrix)
{
    if(!context || !module || !matrix || side<21 || side>53 || (side-21)%4)
        return ESP_ERR_INVALID_SIZE;
    const int scale=QR_SIDE/(side+8);
    if(scale<2) return ESP_ERR_INVALID_SIZE; /* Never unreadable 1px modules. */
    const int start=(QR_SIDE-(side+8)*scale)/2+4*scale;
    esp_err_t privacy=ryz_display_privacy_mark_sensitive();
    if(privacy!=ESP_OK) return privacy;
    tainted=true;
    fault_qr_t *qr=heap_caps_calloc(1,sizeof(*qr),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!qr) return ESP_ERR_NO_MEM;
    qr->image=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,
        .cf=LV_COLOR_FORMAT_A8,.w=QR_SIDE,.h=QR_SIDE,.stride=QR_SIDE},
        .data_size=sizeof(qr->alpha),.data=qr->alpha};
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) if(module(matrix,x,y))
        for(int dy=0;dy<scale;++dy)
            memset(qr->alpha+(start+y*scale+dy)*QR_SIDE+start+x*scale,255,(size_t)scale);
    lv_obj_t *image=ryz_v5_image(context,8,66,&qr->image,V5_WHITE);
    if(!image) { qr_free(qr); return ESP_ERR_NO_MEM; }
    if(!lv_obj_add_event_cb(image,qr_deleted,LV_EVENT_DELETE,qr)) {
        lv_obj_delete(image); qr_free(qr); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool escape(char *out,size_t cap,size_t *used,const char *text)
{
    for(const char *p=text;*p;++p) {
        if(strchr("\\;,:\"",*p)) { if(*used+1>=cap) return false; out[(*used)++]='\\'; }
        if(*used+1>=cap) return false;
        out[(*used)++]=*p;
    }
    out[*used]=0;
    return true;
}

static void text(lv_obj_t *root,int x,int y,int w,int h,const char *value,unsigned size,uint32_t color)
{
    lv_obj_t *label=ryz_v5_label(root,x,y,w,h,value,RYZ_FONT_BODY,size,color,LV_TEXT_ALIGN_LEFT);
    if(label) {
        lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(label,x,y);
        lv_obj_set_size(label,w,h);
        const lv_font_t *font=lv_obj_get_style_text_font(label,0);
        lv_obj_set_style_text_line_space(label,(size==14?20:16)-(int)font->line_height,0);
    }
}

static void button(lv_obj_t *root,int x,int w,const char *label)
{
    ryz_v5_box(root,x,196,w,44,V5_SELECTED,0,0);
    ryz_v5_label(root,x,196,w,44,label,RYZ_FONT_BODY_SEMIBOLD,14,V5_BODY,LV_TEXT_ALIGN_CENTER);
}

esp_err_t ryz_v5_fault_draw(lv_obj_t *root,const ryz_system_ui_snapshot_t *system)
{
    if(!root || !system) return ESP_ERR_INVALID_ARG;
    ryz_v5_box(root,0,0,240,240,V5_BLACK,0,0);
    for(int x=8;x<232;x+=16) ryz_v5_box(root,x,4,12,4,V5_RED,0,0);
    ryz_v5_label(root,8,9,136,34,fault.code[0]?fault.code:"LUA ERROR",
        RYZ_FONT_DISPLAY,32,V5_RED,LV_TEXT_ALIGN_LEFT);
    text(root,148,12,84,30,"SCRIPT\nHALTED",12,V5_MUTED);
    /* Detailed originals belong to the scrollable diagnostic report. */
    ryz_v5_label(root,8,44,224,20,fault.summary,RYZ_FONT_BODY,14,V5_BODY,LV_TEXT_ALIGN_LEFT);
    bool available=link_path[0] && system->ap_active && system->ap_password[0] && system->portal_ip[0];
    char payload[224]={0};
    esp_err_t qr_error=ESP_ERR_INVALID_STATE;
    if(available) {
        if(show_log) snprintf(payload,sizeof(payload),"http://%s%s",system->portal_ip,link_path);
        else {
            snprintf(payload,sizeof(payload),"WIFI:T:WPA;S:");
            size_t length=strlen(payload);
            bool fits=escape(payload,sizeof(payload),&length,system->ap_ssid);
            if(fits && length+4<sizeof(payload)) { memcpy(payload+length,";P:",3); length+=3;
                fits=escape(payload,sizeof(payload),&length,system->ap_password); }
            else fits=false;
            if(fits && length+3<sizeof(payload)) { memcpy(payload+length,";;",3); }
            else available=false;
        }
        if(available) qr_error=ryz_qr_encode_text(payload,qr_matrix,root);
    }
    /* Payload includes a local AP credential; keep no stack copy after encoding. */
    volatile char *wipe=payload;
    for(size_t i=0;i<sizeof(payload);++i) wipe[i]=0;
    available=available && qr_error==ESP_OK;
    if(available) {
        text(root,140,70,92,40,show_log?"SCAN FOR\nDIAGNOSTICS":"JOIN DEVICE\nWI-FI",12,V5_ORANGE);
        if(show_log) {
            text(root,140,116,92,40,fault.source_file,12,V5_BODY);
            char line[24];
            if(fault.line>0) snprintf(line,sizeof(line),"LINE %ld",(long)fault.line);
            else snprintf(line,sizeof(line),"LINE --");
            text(root,140,166,92,20,line,12,V5_MUTED);
        } else text(root,140,116,92,68,"Protected AP\nThen tap NEXT",12,V5_MUTED);
        button(root,8,108,show_log?"JOIN AP":"HOME");
        button(root,124,108,show_log?"HOME":"NEXT");
    } else {
        text(root,12,72,216,24,"LINK UNAVAILABLE",14,V5_MUTED);
        text(root,12,102,216,60,"Protected setup AP and diagnostic service required.",14,V5_MUTED);
        button(root,8,224,"HOME");
        /* Encoding failure is an explicit unavailable view, not a fake QR. */
    }
    presented_link=available; /* Candidate hit geometry; root gates failed commits. */
    return ryz_v5_widgets_status();
}

unsigned ryz_v5_fault_hit(uint16_t x,uint16_t y)
{
    if(x<8 || x>=232 || y<196 || y>=240) return 0;
    if(!presented_link) return 1;
    if(x>=116 && x<124) return 0;
    return (show_log ? x>=124 : x<116)?1:2;
}
void ryz_v5_fault_next(void) { if(link_path[0]) show_log=!show_log; }
