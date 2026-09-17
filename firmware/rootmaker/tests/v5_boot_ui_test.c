/* Exact production LVGL boot renderer with physical panel/clock adapters.
 * No FreeType init, network, device access or alternate UI implementation. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "ryz_v5_boot.h"
#include "ryz_system_ui.h"
#include "ryz_font.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"
#include "lvgl.h"

static uint16_t first[240 * 240], frozen[240 * 240];

static void *wrong_owner(void *unused)
{
    (void)unused;
    assert(ryz_v5_boot_tick(400) == ESP_ERR_INVALID_STATE);
    assert(ryz_v5_boot_timeout() == ESP_ERR_INVALID_STATE);
    assert(ryz_v5_boot_network(NULL) == ESP_ERR_INVALID_STATE);
    assert(ryz_v5_boot_end() == ESP_ERR_INVALID_STATE);
    return NULL;
}

static lv_obj_t *find_label(lv_obj_t *object, const char *text)
{
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return object;
    for (uint32_t i=0;i<lv_obj_get_child_count(object);++i) {
        lv_obj_t *found=find_label(lv_obj_get_child(object,(int32_t)i),text);
        if(found) return found;
    }
    return NULL;
}

static void check_network_statuses(const char *directory)
{
    const struct {
        ryz_provisioning_boot_phase_t phase;
        ryz_provisioning_state_t state;
        bool portal, ip;
        const char *text, *name;
    } cases[]={
        {RYZ_PROVISIONING_BOOT_WAITING,RYZ_PROVISIONING_UNCONFIGURED,false,false,"WIFI STARTING","starting"},
        {RYZ_PROVISIONING_BOOT_OFF,RYZ_PROVISIONING_OFF,false,false,"WIFI OFF","off"},
        {RYZ_PROVISIONING_BOOT_SCANNING,RYZ_PROVISIONING_CONNECTING,false,false,"WIFI SCANNING","scanning"},
        {RYZ_PROVISIONING_BOOT_CONNECTING,RYZ_PROVISIONING_CONNECTING,false,false,"WIFI CONNECTING","connecting"},
        {RYZ_PROVISIONING_BOOT_CONNECTED,RYZ_PROVISIONING_ONLINE,false,true,"WIFI CONNECTED","connected"},
        {RYZ_PROVISIONING_BOOT_CONNECTED,RYZ_PROVISIONING_ONLINE,false,false,"WIFI CONNECT FAILED","no-ip"},
        {RYZ_PROVISIONING_BOOT_NOT_FOUND,RYZ_PROVISIONING_FAILED,false,false,"SSID NOT FOUND / SKIP","skip"},
        {RYZ_PROVISIONING_BOOT_NOT_FOUND,RYZ_PROVISIONING_AP_READY,true,true,"SSID NOT FOUND / AP","missing-ap"},
        {RYZ_PROVISIONING_BOOT_SCAN_FAILED,RYZ_PROVISIONING_AP_READY,true,true,"WIFI SCAN FAILED","scan-failed"},
        {RYZ_PROVISIONING_BOOT_CONNECT_FAILED,RYZ_PROVISIONING_AP_READY,true,true,"WIFI CONNECT FAILED","connect-failed"},
        {RYZ_PROVISIONING_BOOT_NO_CREDENTIALS,RYZ_PROVISIONING_AP_STARTING,false,false,"WIFI STARTING AP","ap-starting"},
        {RYZ_PROVISIONING_BOOT_NO_CREDENTIALS,RYZ_PROVISIONING_AP_READY,true,true,"WIFI AP READY","ap-ready"},
        {RYZ_PROVISIONING_BOOT_NO_CREDENTIALS,RYZ_PROVISIONING_FAILED,false,false,"WIFI AP FAILED","ap-failed"},
    };
    size_t status_heap=0;
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        ryz_provisioning_snapshot_t view={.boot_phase=cases[i].phase,.state=cases[i].state,
            .portal_active=cases[i].portal};
        if(cases[i].ip) { strcpy(view.ipv4,"192.0.2.1"); strcpy(view.portal_ip,"192.0.2.2"); }
        size_t before=ryz_host_display_blit_pixels();
        assert(ryz_v5_boot_network(&view)==ESP_OK);
        assert(ryz_host_display_blit_pixels()-before<=216*18);
        lv_obj_t *label=find_label(lv_screen_active(),cases[i].text);
        assert(label);
        lv_area_t box; lv_obj_get_coords(label,&box);
        assert(box.x1==12 && box.x2==227 && box.y1>=212 && box.y2<232);
        lv_point_t size;
        const lv_font_t *font=lv_obj_get_style_text_font(label,0);
        lv_text_get_size(&size,cases[i].text,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        assert(size.x<=216 && size.y<=18); /* No hidden clip or wrapping. */
        for(const char *c=cases[i].text;*c;++c) {
            lv_font_glyph_dsc_t glyph;
            assert(lv_font_get_glyph_dsc(font,&glyph,(uint8_t)*c,0));
            assert(!glyph.is_placeholder);
        }
        const uint16_t *frame=ryz_host_display_frame();
        for(unsigned y=0;y<240;++y) {
            if(y>=212 && y<230) continue;
            assert(!memcmp(frame+y*240,first+y*240,240*sizeof(uint16_t)));
        }
        if(!i) status_heap=ryz_host_heap_bytes();
        assert(ryz_host_heap_bytes()==status_heap);
        const size_t shows=ryz_host_display_shows();
        assert(ryz_v5_boot_network(&view)==ESP_OK);
        assert(ryz_host_display_shows()==shows); /* Identical state is a no-op. */
        char name[48]; snprintf(name,sizeof(name),"boot-wifi-%s",cases[i].name);
        ryz_host_display_save(directory,name);
    }
    assert(ryz_v5_boot_network(NULL)==ESP_OK);
    assert(find_label(lv_screen_active(),"WIFI UNAVAILABLE"));
    ryz_host_display_save(directory,"boot-wifi-unavailable");
    assert(!ryz_font_ready());
}

static void check_frame(unsigned elapsed)
{
    unsigned phase = (elapsed % 1600 + 400) % 1600;
    unsigned position = ((phase <= 800 ? phase : 1600 - phase) * 60) / 800;
    const uint16_t *frame = ryz_host_display_frame();
    for (unsigned y = 0; y < 240; ++y) {
        for (unsigned x = 0; x < 240; ++x) {
            if (x >= 80 && x < 160 && y >= 234 && y < 236) {
                const uint16_t expected = x >= 80 + position && x < 100 + position ? 0xfb40 : 0x39c7;
                assert(frame[y * 240 + x] == expected);
            } else {
                assert(frame[y * 240 + x] == first[y * 240 + x]);
            }
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(!ryz_font_ready());
    ryz_host_display_reset();
    assert(ryz_v5_boot_tick(0) == ESP_ERR_INVALID_STATE);
    assert(ryz_v5_boot_end() == ESP_OK);
    assert(ryz_v5_boot_begin() == ESP_OK);
    assert(ryz_host_display_shows() > 0);
    assert(ryz_v5_boot_begin() == ESP_ERR_INVALID_STATE);
    memcpy(first, ryz_host_display_frame(), sizeof(first));
    ryz_host_display_save(argv[1], "boot-0000");
    check_frame(0);
    pthread_t task;
    assert(pthread_create(&task, NULL, wrong_owner, NULL) == 0);
    assert(pthread_join(task, NULL) == 0);
    assert(memcmp(first, ryz_host_display_frame(), sizeof(first)) == 0);
    const size_t heap_bytes = ryz_host_heap_bytes();
    for (unsigned t = 20; t <= 3200; t += 20) {
        ryz_host_display_advance_ms(20);
        const size_t previous_pixels = ryz_host_display_blit_pixels();
        assert(ryz_v5_boot_tick(t) == ESP_OK);
        check_frame(t);
        /* Dirty rendering, not merely unchanged output from a full repaint. */
        assert(ryz_host_display_blit_pixels() - previous_pixels <= 80 * 2);
        assert(ryz_host_heap_bytes() == heap_bytes);
        if (t <= 1600 && t % 100 == 0) {
            char name[32];
            snprintf(name, sizeof(name), "boot-%04u", t);
            ryz_host_display_save(argv[1], name);
        }
    }
    assert(!ryz_font_ready());
    assert(memcmp(first, ryz_host_display_frame(), sizeof(first)) == 0);
    check_network_statuses(argv[1]);
    assert(ryz_v5_boot_timeout() == ESP_OK);
    ryz_host_display_save(argv[1], "boot-timeout");
    memcpy(frozen, ryz_host_display_frame(), sizeof(frozen));
    assert(memcmp(first, frozen, sizeof(first)) != 0);
    ryz_host_display_advance_ms(400);
    assert(ryz_v5_boot_tick(3600) == ESP_OK);
    assert(memcmp(frozen, ryz_host_display_frame(), sizeof(frozen)) == 0);
    size_t shows = ryz_host_display_shows();
    assert(ryz_v5_boot_end() == ESP_OK);
    assert(ryz_host_display_shows() == shows); /* No black clear on transition. */
    assert(memcmp(frozen, ryz_host_display_frame(), sizeof(frozen)) == 0);
    assert(ryz_v5_boot_tick(0) == ESP_ERR_INVALID_STATE);
    /* Match start_system_shell: initialize the normal navigation generation
     * after boot lease release, before publishing its first HOME frame. */
    assert(ryz_system_ui_reset_checked() == ESP_OK);
    const ryz_system_ui_snapshot_t home = {0};
    assert(ryz_system_ui_render(&home) == ESP_OK);
    ryz_host_display_save(argv[1], "boot-home");
    assert(memcmp(frozen, ryz_host_display_frame(), sizeof(frozen)) != 0);
    assert(ryz_system_ui_reset_checked() == ESP_OK);
    /* First-frame transfer failure returns the actual error and frees lease. */
    ryz_host_display_fail_show(ESP_FAIL, 16);
    assert(ryz_v5_boot_begin() == ESP_FAIL);
    assert(ryz_v5_boot_tick(0) == ESP_ERR_INVALID_STATE);
    assert(ryz_v5_boot_begin() == ESP_OK);
    ryz_host_display_fail_blit(ESP_FAIL);
    ryz_host_display_advance_ms(400);
    assert(ryz_v5_boot_tick(400) == ESP_FAIL);
    assert(ryz_v5_boot_end() == ESP_OK);
    assert(ryz_system_ui_render(&home) == ESP_OK);
    assert(ryz_system_ui_reset_checked() == ESP_OK);
    assert(!ryz_font_ready());
    printf("V5_BOOT_PASS: two full loops; static-font Wi-Fi states; footer-only dirty updates; same-owner HOME; timeout freeze; flush recovery; no FreeType\n");
    return 0;
}
