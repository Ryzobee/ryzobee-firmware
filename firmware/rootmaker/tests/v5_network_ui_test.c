/* Same-source original-V5 pages, LVGL/FreeType and managed QR algorithm.
 * Only network model, OS/heap/panel and encoder admission are controlled.
 * No network request, device access or phone-scanning claim is made here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ryz_v5_network.h"
#include "ryz_v5_widgets.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"
#include "ryz_qr_encoder.h"
#include "../managed_components/espressif__qrcode/qrcodegen.h"

#ifdef NDEBUG
#error "V5 network checks require active assertions"
#endif

static char encoded[224];
static unsigned encodes, cases;
static esp_err_t encoder_error;
static size_t shared_blocks,shared_bytes;

static bool qr_module(const void *matrix, int x, int y)
{ return qrcodegen_getModule(matrix, x, y); }

esp_err_t ryz_qr_encode_text(const char *text, ryz_qr_matrix_consumer_t consume,
                             void *context)
{
    assert(text && strlen(text) < sizeof(encoded));
    strcpy(encoded, text);
    ++encodes;
    if (encoder_error != ESP_OK) return encoder_error;
    uint8_t matrix[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    uint8_t scratch[sizeof(matrix)];
    if (!qrcodegen_encodeText(text, scratch, matrix, qrcodegen_Ecc_MEDIUM,
                              1, 10, qrcodegen_Mask_AUTO, true)) return ESP_FAIL;
    return consume(context, qrcodegen_getSize(matrix), qr_module, matrix);
}

static void passed(const char *name) { ++cases; printf("PASS %s\n", name); }

static ryz_v5_network_model_t model(ryz_v5_network_page_t page)
{
    ryz_v5_network_model_t result = {.page=page, .enabled=true,
        .can_cancel=true,.can_retry=true,.can_setup=true};
    result.system.connected = page == RYZ_V5_NETWORK_CONNECTED ||
        page == RYZ_V5_NETWORK_INFO || page == RYZ_V5_NETWORK_READY ||
        page == RYZ_V5_NETWORK_FILE_SERVER;
    result.system.state = result.system.connected ? RYZ_SYSTEM_UI_NETWORK_ONLINE :
        page == RYZ_V5_NETWORK_OFF ? RYZ_SYSTEM_UI_NETWORK_OFF :
        page == RYZ_V5_NETWORK_FAILED ? RYZ_SYSTEM_UI_NETWORK_FAILED :
        page == RYZ_V5_NETWORK_JOINING ? RYZ_SYSTEM_UI_NETWORK_CONNECTING :
        RYZ_SYSTEM_UI_NETWORK_AP_READY;
    result.system.configured = page != RYZ_V5_NETWORK_NO_NETWORK;
    if (page == RYZ_V5_NETWORK_OFF) result.enabled = false;
    result.system.ap_active = page == RYZ_V5_NETWORK_AP;
    strcpy(result.system.ap_ssid, "UNIT;AP");
    strcpy(result.system.ap_password, "unit:password");
    strcpy(result.system.portal_ip, "192.168.4.1");
    strcpy(result.sta_ssid, "UNIT_NETWORK");
    if (result.system.connected) strcpy(result.ipv4, "192.0.2.37");
    return result;
}

static lv_obj_t *candidate(const ryz_v5_network_model_t *value)
{
    lv_obj_t *root = NULL;
    assert(ryz_lvgl_system_create(&root) == ESP_OK && root);
    ryz_v5_widgets_begin();
    assert(ryz_v5_network_draw(root, value) == ESP_OK);
    assert(ryz_v5_widgets_status() == ESP_OK);
    return root;
}

static void commit(lv_obj_t *root)
{
    bool cancelled = true;
    assert(ryz_lvgl_system_commit(root, NULL, NULL, &cancelled) == ESP_OK && !cancelled);
    assert(ryz_v5_widgets_status() == ESP_OK);
}

static void release(void)
{
    assert(ryz_lvgl_system_release() == ESP_OK);
    ryz_v5_widgets_release();
    assert(ryz_host_heap_blocks()==shared_blocks && ryz_host_heap_bytes()==shared_bytes);
}

static bool has_label(lv_obj_t *root, const char *text)
{
    for (uint32_t i=0; i<lv_obj_get_child_count(root); ++i) {
        lv_obj_t *child=lv_obj_get_child(root,(int32_t)i);
        if (lv_obj_check_type(child,&lv_label_class) &&
            !strcmp(lv_label_get_text(child),text)) return true;
    }
    return false;
}

static const lv_image_dsc_t *qr_image(lv_obj_t *root)
{
    const lv_image_dsc_t *result=NULL;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *o=lv_obj_get_child(root,(int32_t)i);
        if(!lv_obj_check_type(o,&lv_image_class)) continue;
        const lv_image_dsc_t *image=lv_image_get_src(o);
        if(image->header.w!=152 || image->header.h!=152) continue;
        assert(!result); result=image;
    }
    return result;
}

static void actions(void)
{
    ryz_v5_network_model_t m=model(RYZ_V5_NETWORK_CONNECTED);
    assert(ryz_v5_network_hit(&m,196,37)==RYZ_V5_NETWORK_ACTION_OFF);
    assert(ryz_v5_network_hit(&m,232,37)==RYZ_V5_NETWORK_ACTION_NONE);
    assert(ryz_v5_network_hit(&m,196,55)==RYZ_V5_NETWORK_ACTION_NONE);
    assert(ryz_v5_network_hit(&m,8,106)==RYZ_V5_NETWORK_ACTION_INFO);
    assert(ryz_v5_network_hit(&m,8,140)==RYZ_V5_NETWORK_ACTION_REPROVISION);
    m.can_setup=false;
    assert(ryz_v5_network_hit(&m,8,140)==RYZ_V5_NETWORK_ACTION_NONE);
    m.busy=true;
    assert(ryz_v5_network_hit(&m,210,45)==RYZ_V5_NETWORK_ACTION_NONE);
    m=model(RYZ_V5_NETWORK_OFF);
    assert(ryz_v5_network_hit(&m,210,45)==RYZ_V5_NETWORK_ACTION_ON);
    m=model(RYZ_V5_NETWORK_JOINING);
    assert(ryz_v5_network_hit(&m,56,216)==RYZ_V5_NETWORK_ACTION_CANCEL);
    m.can_cancel=false;
    assert(ryz_v5_network_hit(&m,56,216)==RYZ_V5_NETWORK_ACTION_NONE);
    m=model(RYZ_V5_NETWORK_FAILED);
    assert(ryz_v5_network_hit(&m,56,174)==RYZ_V5_NETWORK_ACTION_BACK);
    assert(ryz_v5_network_hit(&m,116,174)==RYZ_V5_NETWORK_ACTION_RETRY);
    m.can_retry=false;
    assert(ryz_v5_network_hit(&m,116,174)==RYZ_V5_NETWORK_ACTION_NONE);
    m=model(RYZ_V5_NETWORK_READY);
    assert(ryz_v5_network_hit(&m,64,190)==RYZ_V5_NETWORK_ACTION_HOME);
    m=model(RYZ_V5_NETWORK_NO_NETWORK);
    assert(ryz_v5_network_hit(&m,28,180)==RYZ_V5_NETWORK_ACTION_REPROVISION);
    for(unsigned y=164;y<224;++y) for(unsigned x=12;x<228;++x) {
        const bool hit=x>=20 && x<220 && y>=172 && y<216;
        assert(ryz_v5_network_hit(&m,x,y)==(hit?RYZ_V5_NETWORK_ACTION_REPROVISION:RYZ_V5_NETWORK_ACTION_NONE));
    }
    m.busy=true;
    assert(ryz_v5_network_hit(&m,20,172)==RYZ_V5_NETWORK_ACTION_NONE);
    m.busy=false; m.enabled=false;
    assert(ryz_v5_network_hit(&m,219,215)==RYZ_V5_NETWORK_ACTION_NONE);
    m.enabled=true;
    m.can_setup=false;
    assert(ryz_v5_network_hit(&m,20,172)==RYZ_V5_NETWORK_ACTION_NONE);
    assert(ryz_v5_network_hit(&m,28,180)==RYZ_V5_NETWORK_ACTION_NONE);
    for(int p=0;p<=RYZ_V5_NETWORK_NO_NETWORK;++p) {
        m=model(p);
        assert(ryz_v5_network_hit(&m,20,20)==RYZ_V5_NETWORK_ACTION_NONE);
        assert(ryz_v5_network_hit(&m,240,180)==RYZ_V5_NETWORK_ACTION_NONE);
    }
    /* Unsupported forget/reveal/file-server are never disguised mutations. */
    for(int p=RYZ_V5_NETWORK_INFO;p<=RYZ_V5_NETWORK_FILE_SERVER;++p) {
        if(p!=RYZ_V5_NETWORK_INFO && p!=RYZ_V5_NETWORK_FILE_SERVER) continue;
        m=model(p);
        for(unsigned y=32;y<240;++y) for(unsigned x=0;x<240;++x)
            assert(ryz_v5_network_hit(&m,x,y)==RYZ_V5_NETWORK_ACTION_NONE);
    }
    passed("exact hit bounds, real capability/busy gates, unsupported actions and Header exclusion");
}

static void honest_states(void)
{
    ryz_v5_network_model_t m=model(RYZ_V5_NETWORK_NO_NETWORK);
    m.system.configured=true; m.can_setup=false;
    lv_obj_t *root=candidate(&m);
    assert(has_label(root,"Saved network is offline. Check your 2.4 GHz Wi-Fi network."));
    assert(!has_label(root,"NO NETWORK"));
    commit(root); release();
    m=model(RYZ_V5_NETWORK_CONNECTED); m.system.connected=false;
    root=candidate(&m);
    assert(has_label(root,"OFFLINE") && !has_label(root,"ONLINE") && !has_label(root,m.ipv4));
    commit(root); release();
    m=model(RYZ_V5_NETWORK_CONNECTED); m.operation_error=ESP_FAIL;
    strcpy(m.operation_label,"OFF FAILED");
    root=candidate(&m);
    assert(has_label(root,"OFF FAILED") && has_label(root,"ONLINE") && !has_label(root,"RADIO OFF"));
    commit(root); release();
    for(int p=0;p<=RYZ_V5_NETWORK_NO_NETWORK;++p) {
        m=model(p); m.operation_error=ESP_FAIL; strcpy(m.operation_label,"OFF FAILED");
        root=candidate(&m); assert(has_label(root,"OFF FAILED")); commit(root); release();
        m.busy=true;
        root=candidate(&m); assert(has_label(root,"PLEASE WAIT") && !has_label(root,"OFF FAILED"));
        commit(root); release();
    }
    m=model(RYZ_V5_NETWORK_READY); m.system.connected=false;
    root=candidate(&m);
    assert(has_label(root,"NO LINK") && !has_label(root,"CONNECTED"));
    commit(root); release();
    m=model(RYZ_V5_NETWORK_AP); m.system.ap_active=false;
    const unsigned before=encodes;
    root=candidate(&m);
    assert(!qr_image(root) && encodes==before && has_label(root,"WAITING FOR AP"));
    commit(root); release();
    for(int state=RYZ_SYSTEM_UI_NETWORK_UNCONFIGURED;state<=RYZ_SYSTEM_UI_NETWORK_OFF;++state) {
        if(state==RYZ_SYSTEM_UI_NETWORK_AP_READY) continue;
        m=model(RYZ_V5_NETWORK_AP); m.system.state=state; /* Retained AP fields are not proof. */
        root=candidate(&m); assert(!qr_image(root) && encodes==before); commit(root); release();
    }
    for(unsigned condition=0;condition<3;++condition) {
        m=model(RYZ_V5_NETWORK_AP);
        if(condition==0) m.busy=true;
        if(condition==1) m.setup_failed=true;
        if(condition==2) m.system.portal_ip[0]=0;
        root=candidate(&m); assert(!qr_image(root) && encodes==before); commit(root); release();
    }
    m=model(RYZ_V5_NETWORK_JOINING);
    root=candidate(&m); assert(has_label(root,"--s")); commit(root); release();
    m.elapsed_valid=true; m.elapsed_ms=37000;
    root=candidate(&m); assert(has_label(root,"37s")); commit(root); release();
    m=model(RYZ_V5_NETWORK_CONNECTED);
    memset(m.sta_ssid,'x',sizeof(m.sta_ssid));
    assert(ryz_lvgl_system_create(&root)==ESP_OK);
    ryz_v5_widgets_begin();
    assert(ryz_v5_network_draw(root,&m)==ESP_ERR_INVALID_ARG);
    assert(!lv_obj_get_child_count(root)); release();
    assert(ryz_v5_network_hit(&m,8,106)==RYZ_V5_NETWORK_ACTION_NONE);
    passed("saved/offline/pending states stay honest, elapsed is real, malformed strings fail before drawing");
}

static bool cancel_now(void *context) { (void)context; return true; }

static void real_qr(void)
{
    ryz_v5_network_model_t m=model(RYZ_V5_NETWORK_AP);
    lv_obj_t *a=candidate(&m);
    assert(!strcmp(encoded,"WIFI:T:WPA;S:UNIT\\;AP;P:unit\\:password;;"));
    const lv_image_dsc_t *image=qr_image(a);
    assert(image && image->header.cf==LV_COLOR_FORMAT_A8 && image->header.stride==152);
    uint8_t actual[152*152]; memcpy(actual,image->data,sizeof(actual));
    uint8_t matrix[qrcodegen_BUFFER_LEN_FOR_VERSION(10)],scratch[sizeof(matrix)];
    assert(qrcodegen_encodeText(encoded,scratch,matrix,qrcodegen_Ecc_MEDIUM,1,10,qrcodegen_Mask_AUTO,true));
    int side=qrcodegen_getSize(matrix),scale=152/(side+8),start=(152-(side+8)*scale)/2+4*scale;
    for(int y=0;y<152;++y) for(int x=0;x<152;++x) {
        int mx=(x-start)/scale,my=(y-start)/scale;
        bool ink=x>=start && y>=start && mx<side && my<side && qrcodegen_getModule(matrix,mx,my);
        assert(actual[y*152+x]==(ink ? 255 : 0));
    }
    commit(a);
    for(unsigned y=0;y<152;++y) for(unsigned x=0;x<152;++x)
        assert(ryz_host_display_pixel(68+x,42+y)==(actual[y*152+x] ? 0xffff : 0));
    strcpy(m.system.ap_ssid,"OTHER_AP");
    lv_obj_t *b=candidate(&m);
    const lv_image_dsc_t *other=qr_image(b);
    assert(other && other!=image && other->data!=image->data && memcmp(other->data,actual,sizeof(actual)));
    bool cancelled=false;
    assert(ryz_lvgl_system_commit(b,cancel_now,NULL,&cancelled)==ESP_ERR_TIMEOUT && cancelled);
    assert(lv_screen_active()==a && !memcmp(image->data,actual,sizeof(actual)));
    b=candidate(&m);
    ryz_host_display_fail_show(ESP_FAIL,0);
    assert(ryz_lvgl_system_commit(b,NULL,NULL,&cancelled)==ESP_FAIL && !cancelled);
    assert(lv_screen_active()==a && !memcmp(image->data,actual,sizeof(actual)));
    release();
    /* Encoder refusal and QR allocation failure must not become valid frames. */
    assert(ryz_lvgl_system_create(&b)==ESP_OK);
    ryz_v5_widgets_begin(); encoder_error=ESP_ERR_INVALID_SIZE;
    assert(ryz_v5_network_draw(b,&m)==ESP_ERR_INVALID_SIZE && !qr_image(b));
    encoder_error=ESP_OK; release();
    assert(ryz_lvgl_system_create(&b)==ESP_OK);
    ryz_v5_widgets_begin(); ryz_host_heap_fail_after(0);
    assert(ryz_v5_network_draw(b,&m)==ESP_ERR_NO_MEM && !qr_image(b));
    ryz_host_heap_allow(); release();
    b=candidate(&m); commit(b); release();
    passed("real QR modules/transparent quiet-zone/panel pixels, independent candidate lifetime and error propagation");
}

static void pages(const char *directory)
{
    for (int page=RYZ_V5_NETWORK_CONNECTED; page<=RYZ_V5_NETWORK_NO_NETWORK; ++page) {
        ryz_v5_network_model_t value=model(page);
        lv_obj_t *root=candidate(&value);
        assert(lv_obj_get_child_count(root) < 90);
        assert(!has_label(root,"STUDIO_2G") && !has_label(root,"192.168.1.86"));
        commit(root);
        char name[40];
        snprintf(name,sizeof(name),"v5-network-%d",page);
        ryz_host_display_save(directory,name);
        release();
    }
    passed("nine original V5 pages render with real fonts; bounded objects and no sample network data");
}

int main(int argc,char **argv)
{
    assert(argc==2);
    assert(ryz_font_init()==ESP_OK);
    /* Establish only the permanent shared display-buffer baseline before
     * drawing any page/font/QR; do not forgive a leak in the first page. */
    lv_obj_t *empty=NULL;
    assert(ryz_lvgl_system_create(&empty)==ESP_OK && empty);
    assert(ryz_lvgl_system_release()==ESP_OK);
    shared_blocks=ryz_host_heap_blocks(); shared_bytes=ryz_host_heap_bytes();
    assert(shared_blocks==1 && shared_bytes>0);
    pages(argv[1]);
    actions();
    honest_states();
    real_qr();
    printf("V5_NETWORK_UI_PASS cases=%u encodes=%u\n",cases,encodes);
    return 0;
}
