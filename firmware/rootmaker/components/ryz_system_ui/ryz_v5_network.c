#include "ryz_v5_network.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "ryz_qr_encoder.h"
#include "ryz_v5_assets.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_password.h"
#include "src/misc/cache/instance/lv_image_cache.h"

/* Original V5 frame geometry and image ink origins: v5-network-source.json.
 * Device values use C caches, never design sample data. The PASS cell uses
 * the private hold controller, not the ordinary network snapshot. */
#define MONO RYZ_FONT_MONO
#define MONO_M RYZ_FONT_MONO_MEDIUM
#define MONO_SB RYZ_FONT_MONO_SEMIBOLD
#define BODY_M RYZ_FONT_BODY_MEDIUM
#define BODY_SB RYZ_FONT_BODY_SEMIBOLD
#define DISPLAY RYZ_FONT_DISPLAY
#define LEFT LV_TEXT_ALIGN_LEFT
#define RIGHT LV_TEXT_ALIGN_RIGHT
#define CENTER LV_TEXT_ALIGN_CENTER
#define LABEL ryz_v5_label
#define QR_SIZE 152

static bool valid(const ryz_v5_network_model_t *m)
{
    return m && m->page >= RYZ_V5_NETWORK_CONNECTED &&
        m->page <= RYZ_V5_NETWORK_NO_NETWORK &&
        memchr(m->operation_label, 0, sizeof(m->operation_label)) &&
        memchr(m->sta_ssid, 0, sizeof(m->sta_ssid)) &&
        memchr(m->ipv4, 0, sizeof(m->ipv4)) &&
        memchr(m->mac, 0, sizeof(m->mac)) &&
        memchr(m->ipv6, 0, sizeof(m->ipv6)) &&
        memchr(m->system.ap_ssid, 0, sizeof(m->system.ap_ssid)) &&
        memchr(m->system.ap_password, 0, sizeof(m->system.ap_password)) &&
        memchr(m->system.portal_ip, 0, sizeof(m->system.portal_ip));
}

/* UI font assets are ASCII. Preserve all bytes in the QR, but render
 * unsupported/control bytes as '?' instead of markup or missing glyphs. */
static void display_text(char *out, size_t cap, const char *source)
{
    size_t i=0;
    for (; source[i] && i+1<cap; ++i) {
        const unsigned char c=(unsigned char)source[i];
        out[i]=c>=32 && c<=126 ? (char)c : '?';
    }
    out[i]=0;
    if (!i && cap>=3) memcpy(out,"--",3);
}

static lv_obj_t *box(lv_obj_t *root,int x,int y,int w,int h,uint32_t color,int radius)
{
    lv_obj_t *o=ryz_v5_box(root,x,y,w,h,color,0,0);
    if (o && radius) lv_obj_set_style_radius(o,radius,0);
    return o;
}

static void block(lv_obj_t *root,int x,int y,int w,int h,const char *text,
                  unsigned px,uint32_t color,lv_text_align_t align)
{
    if (px < 12) px = 12;
    lv_obj_t *o=LABEL(root,x,y,w,h,text,BODY_M,px,color,align);
    if (!o) return;
    lv_point_t size;
    lv_text_get_size(&size,text,lv_obj_get_style_text_font(o,0),0,0,w,LV_TEXT_FLAG_NONE);
    lv_label_set_long_mode(o,LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(o,x,y+(size.y<h ? (h-size.y)/2 : 0));
    lv_obj_set_height(o,size.y<h ? size.y : h);
}

static void pill(lv_obj_t *root,int x,int y,int w,const char *text,uint32_t color)
{
    box(root,x,y,w,16,V5_SELECTED,0);
    LABEL(root,x,y,w,16,text,MONO_SB,12,color,CENTER);
}

static void button(lv_obj_t *root,int x,int y,int w,const char *text,bool active,bool filled)
{
    box(root,x,y,w,28,active && filled ? V5_ORANGE : V5_SELECTED,2);
    LABEL(root,x,y,w,28,text,BODY_SB,14,
          active ? (filled ? V5_BLACK : V5_BODY) : V5_DISABLED,CENTER);
}

static void footer(lv_obj_t *root,int x,const char *hint)
{
    box(root,x,216,240-x,24,V5_BLACK,0);
    box(root,x,216,240-x,1,V5_BORDER,0);
    if(hint) LABEL(root,x ? 56 : 8,216,x ? 176 : 224,24,hint,MONO_M,12,V5_MUTED,RIGHT);
}

static const char *pending_hint(const ryz_v5_network_model_t *m,const char *idle)
{
    if(m->initialization_error!=ESP_OK) return "INIT FAILED";
    if(m->busy) return m->setup_pending ? "SETUP PENDING" : "PLEASE WAIT";
    if(m->operation_error!=ESP_OK)
        return m->operation_label[0] ? m->operation_label : "NETWORK ERROR";
    if(m->error!=ESP_OK) return "NETWORK ERROR";
    return idle;
}

static void master(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    LABEL(root,8,36,150,20,"WIRELESS NETWORK",MONO_SB,12,V5_MUTED,LEFT);
    box(root,196,37,36,18,m->enabled ? V5_ORANGE : V5_BORDER,9);
    box(root,m->enabled ? 216 : 198,39,14,14,
        m->busy ? V5_DISABLED : m->enabled ? V5_BLACK : V5_BODY,7);
}

static void rail(lv_obj_t *root,unsigned step)
{
    box(root,0,32,48,208,V5_SURFACE,0);
    box(root,47,32,1,208,V5_BORDER,0);
    box(root,8,67,1,96,V5_BORDER,0);
    for(unsigned i=0;i<3;++i) box(root,6,63+(int)i*48,5,5,i==step ? V5_ORANGE : V5_BORDER,0);
    ryz_v5_image(root,15,61,&ryz_v5_asset_network_ap_label,step==0 ? V5_ORANGE : V5_MUTED);
    ryz_v5_image(root,15,109,&ryz_v5_asset_network_join_label,step==1 ? V5_ORANGE : V5_MUTED);
    ryz_v5_image(root,11,157,&ryz_v5_asset_network_ready_label,step==2 ? V5_ORANGE : V5_MUTED);
}

static void row(lv_obj_t *root,int y,const char *name,const char *value,bool selected,bool active)
{
    if(selected) box(root,8,y,224,34,V5_SELECTED,0);
    box(root,8,y,selected ? 3 : 1,34,selected ? V5_ORANGE : V5_BORDER,0);
    box(root,8,y+33,224,1,V5_BORDER,0);
    LABEL(root,16,y,140,34,name,selected ? BODY_SB : BODY_M,strlen(name)>16?12:14,active ? V5_BODY : V5_DISABLED,LEFT);
    LABEL(root,158,y,66,34,value,MONO_SB,14,
          active ? (selected ? V5_ORANGE : V5_MUTED) : V5_DISABLED,RIGHT);
}

static void connected(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    char ssid[33],ip[16];
    display_text(ssid,sizeof(ssid),m->sta_ssid);
    display_text(ip,sizeof(ip),m->system.connected ? m->ipv4 : "");
    master(root,m);
    box(root,8,60,224,42,V5_SELECTED,2);
    box(root,8,60,3,42,m->system.connected ? V5_GREEN : V5_WARNING,0);
    LABEL(root,16,62,138,20,ssid,BODY_SB,13,V5_WHITE,LEFT);
    LABEL(root,16,82,138,16,ip,MONO,12,V5_MUTED,LEFT);
    pill(root,168,72,56,m->system.connected ? "ONLINE" : "OFFLINE",m->system.connected ? V5_GREEN : V5_WARNING);
    row(root,106,"NETWORK INFO","OPEN",true,true);
    row(root,140,"PROVISION","QR",false,m->can_setup && !m->busy);
    row(root,174,"FILE SERVER","N/A",false,false);
    footer(root,0,pending_hint(m,m->system.configured ? "AUTO RECONNECT" : "NOT SAVED"));
}

static void off(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    master(root,m);
    ryz_v5_image(root,112,79,&ryz_v5_asset_network_off,V5_DISABLED);
    LABEL(root,24,118,192,24,m->system.state==RYZ_SYSTEM_UI_NETWORK_OFF ? "RADIO OFF" : "RADIO STOPPING",
          DISPLAY,16,V5_DISABLED,CENTER);
    block(root,24,144,192,34,"Turn on Wi-Fi to use saved networks, setup and file server.",9,V5_DISABLED,CENTER);
    row(root,180,"DEPENDENT SETTINGS","LOCKED",false,false);
    footer(root,0,pending_hint(m,NULL));
}

static int info_fields(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    char ssid[33],ip[16],mac[18],ipv6[40];
    display_text(ssid,sizeof(ssid),m->sta_ssid);
    display_text(ip,sizeof(ip),m->system.connected ? m->ipv4 : "");
    display_text(mac,sizeof(mac),m->system.connected && m->mac_valid ? m->mac : "");
    display_text(ipv6,sizeof(ipv6),m->system.connected && m->ipv6_valid ? m->ipv6 : "");
    const ryz_v5_info_field_t fields[] = {
        {"SSID",ssid},{"MAC",mac},{"IPv4",ip},{"IPv6",ipv6},{"PASS",NULL},
    };
    if (root) {
        lv_obj_t *content = ryz_v5_info_draw(root,fields,5,m->info_scroll);
        if (content) {
            esp_err_t error=ryz_v5_password_draw_at(content,68,
                ryz_v5_info_row_y(fields,4)+8,152);
            if(error!=ESP_OK) return -1;
        }
    }
    return ryz_v5_info_height(fields,5);
}

int ryz_v5_network_info_height(const ryz_v5_network_model_t *m)
{ return m ? info_fields(NULL,m) : 0; }

static esp_err_t info(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    const char *hint=ryz_v5_password_hint();
    const char *heading=m->busy?"PLEASE WAIT":m->operation_label[0]?m->operation_label:
        !strcmp(hint,"ASCII FONT ONLY") || !strcmp(hint,"HOLD AGAIN TO RETRY")?hint:"ACTIVE NETWORK";
    LABEL(root,8,36,154,18,heading,MONO_SB,12,V5_ORANGE,LEFT);
    pill(root,168,37,64,m->system.connected ? "ONLINE" : "OFFLINE",m->system.connected ? V5_GREEN : V5_WARNING);
    if(info_fields(root,m)<0) return ESP_ERR_NO_MEM;
    box(root,8,194,110,42,V5_SELECTED,0);
    LABEL(root,8,194,110,42,"FORGET NETWORK",BODY_SB,12,V5_DISABLED,CENTER);
    const bool enabled=m->enabled && !m->busy && m->system.connected && ryz_v5_password_can_hold();
    box(root,122,194,110,42,enabled && ryz_v5_password_held()?V5_ORANGE:V5_SELECTED,0);
    LABEL(root,122,194,110,42,ryz_v5_password_held()?"RELEASE TO HIDE":"HOLD TO VIEW",BODY_SB,14,
        !enabled?V5_DISABLED:ryz_v5_password_held()?V5_BLACK:V5_BODY,CENTER);
    return ryz_v5_widgets_status();
}

typedef struct { lv_image_dsc_t image; uint8_t alpha[QR_SIZE*QR_SIZE]; } qr_image_t;

static void qr_deleted(lv_event_t *event)
{
    qr_image_t *qr=lv_event_get_user_data(event);
    lv_image_cache_drop(&qr->image);
    heap_caps_free(qr);
}

static esp_err_t qr_matrix(void *context,int side,ryz_qr_module_reader_t module,const void *matrix)
{
    lv_obj_t *root=context;
    if(!root || !module || !matrix || side<21 || side>57 || (side-21)%4) return ESP_ERR_INVALID_SIZE;
    const int scale=QR_SIZE/(side+8),start=(QR_SIZE-(side+8)*scale)/2+4*scale;
    if(!scale) return ESP_ERR_INVALID_SIZE;
    qr_image_t *qr=heap_caps_calloc(1,sizeof(*qr),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!qr) return ESP_ERR_NO_MEM;
    qr->image=(lv_image_dsc_t){
        .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.w=QR_SIZE,.h=QR_SIZE,.stride=QR_SIZE},
        .data_size=sizeof(qr->alpha),.data=qr->alpha,
    };
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        if(!module(matrix,x,y)) continue;
        for(int dy=0;dy<scale;++dy)
            memset(&qr->alpha[(start+y*scale+dy)*QR_SIZE+start+x*scale],255,(size_t)scale);
    }
    lv_obj_t *o=ryz_v5_image(root,68,42,&qr->image,V5_WHITE);
    if(!o) { heap_caps_free(qr); return ESP_ERR_NO_MEM; }
    if(!lv_obj_add_event_cb(o,qr_deleted,LV_EVENT_DELETE,qr)) {
        lv_obj_delete(o);
        lv_image_cache_drop(&qr->image);
        heap_caps_free(qr);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool escaped(char *out,size_t cap,size_t *length,const char *input)
{
    for(const unsigned char *p=(const unsigned char *)input;*p;++p) {
        const bool special=*p=='\\' || *p==';' || *p==',' || *p=='"' || *p==':';
        if(*length+(special ? 2u : 1u)>=cap) return false;
        if(special) out[(*length)++]='\\';
        out[(*length)++]=(char)*p;
    }
    out[*length]=0;
    return true;
}

static esp_err_t ap(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    rail(root,0);
    const char *operation=pending_hint(m,NULL);
    static const int brackets[][4]={
        {66,40,18,2},{66,40,2,18},{204,40,18,2},{220,40,2,18},
        {66,194,18,2},{66,178,2,18},{204,194,18,2},{220,178,2,18},
    };
    for(unsigned i=0;i<8;++i) box(root,brackets[i][0],brackets[i][1],brackets[i][2],brackets[i][3],V5_ORANGE,0);
    const bool ready=m->enabled && !m->busy && !m->setup_failed && m->system.ap_active &&
        m->system.state==RYZ_SYSTEM_UI_NETWORK_AP_READY && m->system.portal_ip[0] &&
        m->system.ap_ssid[0] && m->system.ap_password[0];
    if(ready && !operation) {
        char payload[224]="WIFI:T:WPA;S:";
        size_t length=strlen(payload);
        if(!escaped(payload,sizeof(payload),&length,m->system.ap_ssid) || length+3>=sizeof(payload)) return ESP_ERR_INVALID_SIZE;
        memcpy(payload+length,";P:",4); length+=3;
        if(!escaped(payload,sizeof(payload),&length,m->system.ap_password) || length+2>=sizeof(payload)) return ESP_ERR_INVALID_SIZE;
        memcpy(payload+length,";;",3);
        esp_err_t error=ryz_qr_encode_text(payload,qr_matrix,root);
        memset(payload,0,sizeof(payload));
        if(error!=ESP_OK) return error;
    } else block(root,78,94,132,52,operation ? operation : m->error!=ESP_OK ? "AP UNAVAILABLE" : "WAITING FOR AP",12,V5_MUTED,CENTER);
    char ssid[33],ip[16];
    display_text(ssid,sizeof(ssid),ready ? m->system.ap_ssid : "");
    display_text(ip,sizeof(ip),ready ? m->system.portal_ip : "");
    LABEL(root,78,198,34,16,"SSID",BODY_SB,12,V5_ORANGE,LEFT);
    LABEL(root,116,198,94,16,ssid,BODY_M,12,V5_WHITE,LEFT);
    LABEL(root,78,219,34,16,"IP",BODY_SB,12,V5_MUTED,LEFT);
    LABEL(root,116,219,94,16,ip,BODY_M,12,V5_MUTED,LEFT);
    return ryz_v5_widgets_status();
}

static void joining(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    char ssid[33],elapsed[24];
    display_text(ssid,sizeof(ssid),m->sta_ssid);
    rail(root,1);
    pill(root,152,38,80,m->busy ? "PLEASE WAIT" : "CONNECTING",V5_ORANGE);
    box(root,100,66,88,88,V5_BORDER,44);
    box(root,108,74,72,72,V5_BLACK,36);
    box(root,140,62,8,24,V5_ORANGE,4);
    LABEL(root,68,92,152,28,ssid,MONO_SB,14,V5_WHITE,CENTER);
    block(root,56,160,176,36,"Getting credentials\nConnecting to 2.4 GHz Wi-Fi",9,V5_MUTED,CENTER);
    /* Association has no completion percentage; the design's 106px sample
     * progress is not evidence. Preserve its track without false progress. */
    box(root,56,204,176,1,V5_BORDER,0);
    footer(root,48,NULL);
    LABEL(root,56,216,80,24,"CANCEL",BODY_SB,12,m->can_cancel && !m->busy ? V5_ORANGE : V5_DISABLED,LEFT);
    if(m->elapsed_valid) snprintf(elapsed,sizeof(elapsed),"%lus",(unsigned long)(m->elapsed_ms/1000));
    else memcpy(elapsed,"--s",4);
    LABEL(root,144,216,88,24,pending_hint(m,elapsed),MONO_M,12,V5_MUTED,RIGHT);
}

static void failed(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    char error[24];
    rail(root,m->setup_failed ? 0 : 1);
    pill(root,56,38,128,m->setup_failed ? "SETUP FAILED" : "CONNECTION FAILED",V5_RED);
    if(m->setup_failed) snprintf(error,sizeof(error),"%ld",(long)m->operation_error);
    else if(m->failure_detail) snprintf(error,sizeof(error),"%ld",(long)m->failure_detail);
    else if(m->error!=ESP_OK) snprintf(error,sizeof(error),"%ld",(long)m->error);
    else memcpy(error,"--",3);
    LABEL(root,188,38,44,16,error,MONO_SB,12,V5_RED,RIGHT);
    box(root,56,62,176,88,V5_SELECTED,2);
    box(root,56,62,3,88,V5_RED,0);
    LABEL(root,66,68,156,24,m->setup_failed ? "CHECK AP SETUP" : "CHECK NETWORK",DISPLAY,16,V5_WHITE,LEFT);
    if(m->setup_failed) {
        /* Same V5 hint slots. AP/OTA cleanup failure is not evidence that
         * the saved password, router band or signal is wrong. */
        LABEL(root,64,94,166,18,"- AP could not start",BODY_M,12,V5_BODY,LEFT);
        LABEL(root,64,114,166,18,"- Saved network kept",BODY_M,12,V5_WARNING,LEFT);
        LABEL(root,64,134,166,16,"- Retry to open the AP",BODY_M,12,V5_MUTED,LEFT);
    } else {
        ryz_v5_image(root,64,97,&ryz_v5_asset_network_fail_hint1,V5_BODY);
        ryz_v5_image(root,64,117,&ryz_v5_asset_network_fail_hint2,V5_WARNING);
        ryz_v5_image(root,64,136,&ryz_v5_asset_network_fail_hint3,V5_MUTED);
    }
    button(root,56,174,56,"EXIT",true,false);
    button(root,116,174,116,"RETRY",m->can_retry && !m->busy,true);
    footer(root,48,pending_hint(m,m->system.configured ? "SAVED NETWORK KEPT" : "NO NETWORK SAVED"));
}

static void ready(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    char ssid[33],ip[16],detail[64];
    display_text(ssid,sizeof(ssid),m->sta_ssid);
    display_text(ip,sizeof(ip),m->system.connected ? m->ipv4 : "");
    snprintf(detail,sizeof(detail),"%s\n%s",ssid,ip);
    rail(root,2);
    pill(root,56,38,80,m->system.connected ? "CONNECTED" : "OFFLINE",m->system.connected ? V5_GREEN : V5_WARNING);
    if(m->system.connected) ryz_v5_image(root,127,77,&ryz_v5_asset_network_ready,V5_GREEN);
    LABEL(root,48,128,192,26,m->system.connected ? "WI-FI READY" : "NO LINK",DISPLAY,22,V5_WHITE,CENTER);
    lv_obj_t *details=LABEL(root,56,156,176,32,detail,MONO,12,V5_BODY,CENTER);
    if(details) { lv_obj_set_pos(details,56,156); lv_obj_set_height(details,32); }
    box(root,64,190,160,24,V5_ORANGE,2);
    LABEL(root,64,190,160,24,"GO TO HOME",BODY_SB,14,V5_BLACK,CENTER);
    footer(root,48,pending_hint(m,m->system.configured ? "AUTO SAVED" : "NOT SAVED"));
}

static void file_server(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    LABEL(root,8,36,162,20,"WIRELESS FILE SERVER",MONO_SB,12,V5_MUTED,LEFT);
    box(root,196,37,36,18,V5_BORDER,9);
    box(root,198,39,14,14,V5_DISABLED,7);
    pill(root,8,58,60,"N/A",V5_DISABLED);
    LABEL(root,76,58,156,16,"--",MONO_M,12,V5_DISABLED,LEFT);
    LABEL(root,12,84,71,71,"N/A",MONO_M,12,V5_DISABLED,CENTER);
    box(root,92,84,140,72,V5_SELECTED,0);
    LABEL(root,104,88,116,22,"DROP FILES",DISPLAY,15,V5_DISABLED,LEFT);
    block(root,100,110,124,48,"Upload to device FS\nDownload / delete files",12,V5_DISABLED,LEFT);
    LABEL(root,8,164,224,18,"FILE SERVER UNAVAILABLE",BODY_M,12,V5_MUTED,CENTER);
    button(root,8,184,224,"OPEN FILE SERVER",false,true);
    footer(root,0,pending_hint(m,"UNAVAILABLE"));
}

static void no_network(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    const bool init_failed=m->initialization_error!=ESP_OK;
    master(root,m);
    pill(root,8,62,92,init_failed ? "UNAVAILABLE" :
         m->system.configured ? "OFFLINE" : "NO NETWORK",V5_WARNING);
    LABEL(root,24,88,192,34,init_failed ? "WI-FI UNAVAILABLE" : "NOT CONNECTED",DISPLAY,22,V5_WHITE,CENTER);
    block(root,28,124,184,48,init_failed ?
          "Wi-Fi could not initialize.\nSaved network state is unknown." : m->system.configured ?
          "Saved network is offline. Check your 2.4 GHz Wi-Fi network." :
          "No saved network. Start setup and choose a 2.4 GHz Wi-Fi network.",9,V5_BODY,CENTER);
    button(root,28,180,184,"START WI-FI SETUP",m->enabled && m->can_setup && !m->busy,true);
    footer(root,0,pending_hint(m,m->can_setup ? "OPENS AP QR" : "SETUP UNAVAILABLE"));
}

esp_err_t ryz_v5_network_draw(lv_obj_t *root,const ryz_v5_network_model_t *m)
{
    if(!root || !valid(m)) return ESP_ERR_INVALID_ARG;
    box(root,0,32,240,208,V5_BLACK,0);
    switch(m->page) {
    case RYZ_V5_NETWORK_CONNECTED: connected(root,m); break;
    case RYZ_V5_NETWORK_OFF: off(root,m); break;
    case RYZ_V5_NETWORK_INFO: return info(root,m);
    case RYZ_V5_NETWORK_AP: return ap(root,m);
    case RYZ_V5_NETWORK_JOINING: joining(root,m); break;
    case RYZ_V5_NETWORK_FAILED:
        if(m->setup_pending) return ap(root,m);
        failed(root,m); break;
    case RYZ_V5_NETWORK_READY: ready(root,m); break;
    case RYZ_V5_NETWORK_FILE_SERVER: file_server(root,m); break;
    case RYZ_V5_NETWORK_NO_NETWORK: no_network(root,m); break;
    }
    return ryz_v5_widgets_status();
}

static bool inside(uint16_t x,uint16_t y,int left,int top,int w,int h)
{ return x>=left && y>=top && x<left+w && y<top+h; }

ryz_v5_network_action_t ryz_v5_network_hit(const ryz_v5_network_model_t *m,uint16_t x,uint16_t y)
{
    if(!valid(m) || x>=240 || y<32 || y>=240) return RYZ_V5_NETWORK_ACTION_NONE;
    if((m->page==RYZ_V5_NETWORK_CONNECTED || m->page==RYZ_V5_NETWORK_OFF || m->page==RYZ_V5_NETWORK_NO_NETWORK) &&
       !m->busy && inside(x,y,196,37,36,18)) return m->enabled ? RYZ_V5_NETWORK_ACTION_OFF : RYZ_V5_NETWORK_ACTION_ON;
    switch(m->page) {
    case RYZ_V5_NETWORK_INFO:
        if(m->enabled && !m->busy && m->system.connected &&
           ryz_v5_password_can_hold() && inside(x,y,122,194,110,42))
            return RYZ_V5_NETWORK_ACTION_HOLD_PASSWORD;
        break;
    case RYZ_V5_NETWORK_CONNECTED:
        if(inside(x,y,8,106,224,34)) return RYZ_V5_NETWORK_ACTION_INFO;
        if(m->can_setup && !m->busy && inside(x,y,8,140,224,34)) return RYZ_V5_NETWORK_ACTION_REPROVISION;
        break;
    case RYZ_V5_NETWORK_JOINING:
        if(m->can_cancel && !m->busy && inside(x,y,56,216,80,24)) return RYZ_V5_NETWORK_ACTION_CANCEL;
        break;
    case RYZ_V5_NETWORK_FAILED:
        if(inside(x,y,56,174,56,28)) return RYZ_V5_NETWORK_ACTION_BACK;
        if(m->can_retry && !m->busy && inside(x,y,116,174,116,28)) return RYZ_V5_NETWORK_ACTION_RETRY;
        break;
    case RYZ_V5_NETWORK_READY:
        if(inside(x,y,64,190,160,24)) return RYZ_V5_NETWORK_ACTION_HOME;
        break;
    case RYZ_V5_NETWORK_NO_NETWORK:
        /* Keep the 184x28 artwork; add 8px touch slop without entering the footer. */
        if(m->enabled && m->can_setup && !m->busy && inside(x,y,20,172,200,44)) return RYZ_V5_NETWORK_ACTION_REPROVISION;
        break;
    default: break;
    }
    return RYZ_V5_NETWORK_ACTION_NONE;
}

const char *ryz_v5_network_title(ryz_v5_network_page_t page)
{
    static const char *const names[]={"WI-FI","WI-FI","NETWORK","AP LINK","JOIN","JOIN","READY","SERVER","WI-FI"};
    return page>=RYZ_V5_NETWORK_CONNECTED && page<=RYZ_V5_NETWORK_NO_NETWORK ? names[page] : "WI-FI";
}
