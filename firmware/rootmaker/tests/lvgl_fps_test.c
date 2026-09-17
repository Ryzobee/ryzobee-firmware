#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "ryz_lvgl.h"
#include "ryz_lvgl_system.h"
#include "ryz_display_host.h"

#ifdef NDEBUG
#error "FPS checks require assertions"
#endif

/* Controlled Host time, real bridge + LVGL sysmon, not a panel benchmark. */
static void *other_owner(void *unused)
{
    (void)unused;
    uint16_t fps = 99;
    assert(!ryz_lvgl_get_fps(&fps) && fps == 0);
    return NULL;
}

int main(void)
{
    assert(LV_USE_SYSMON && LV_USE_PERF_MONITOR);
    uint16_t fps = 99;
    assert(!ryz_lvgl_get_fps(&fps) && fps == 0);
    assert(!ryz_lvgl_get_fps(NULL));
    ryz_host_display_reset();
    /* Initialization at nonzero uptime must not dilute the first window. */
    ryz_host_display_advance_ms(5000);
    lv_obj_t *root = NULL;
    assert(ryz_lvgl_system_create(&root) == ESP_OK);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x123456), 0);
    assert(ryz_lvgl_system_commit(root, NULL, NULL, NULL) == ESP_OK);
    const size_t shows = ryz_host_display_shows();
    for (unsigned i = 0; i < 9; ++i) {
        ryz_host_display_advance_ms(100);
        assert(ryz_lvgl_system_pump(NULL, NULL, NULL) == ESP_OK);
        assert(!ryz_lvgl_get_fps(&fps));
    }
    ryz_host_display_advance_ms(99);
    assert(!ryz_lvgl_get_fps(&fps));
    ryz_host_display_advance_ms(1);
    assert(ryz_lvgl_get_fps(&fps) && fps == 10);
    /* Ten LVGL refresh cycles, but only the first one transferred pixels. */
    assert(ryz_host_display_shows() == shows);
    for (unsigned y = 0; y < 240; ++y)
        for (unsigned x = 0; x < 240; ++x)
            assert(ryz_host_display_pixel(x, y) == 0x11aa);
    puts("PASS official refresh-cycle FPS, one-second first sample, no overlay");

    /* Official sysmon clears its working structure after publishing. The
     * bridge must retain the published number, not poll that zeroed field. */
    ryz_host_display_advance_ms(999);
    assert(ryz_lvgl_system_pump(NULL, NULL, NULL) == ESP_OK);
    assert(ryz_lvgl_get_fps(&fps) && fps == 10);
    ryz_host_display_advance_ms(1);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, other_owner, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(ryz_lvgl_get_fps(&fps) && fps == 1);
    ryz_host_display_advance_ms(1000);
    assert(ryz_lvgl_get_fps(&fps) && fps == 0);
    puts("PASS cached sample, quiet zero, cross-owner rejection");

    /* A manually driven 100 Hz refresh still uses LVGL's official 33 ms
     * configuration cap, not a separately implemented counter formula. */
    for (unsigned i = 0; i < 100; ++i) {
        ryz_host_display_advance_ms(10);
        lv_timer_ready(lv_display_get_refr_timer(lv_obj_get_display(root)));
        assert(ryz_lvgl_system_pump(NULL, NULL, NULL) == ESP_OK);
    }
    assert(ryz_lvgl_get_fps(&fps) && fps == 30);
    /* Delay uses elapsed time, not an assumed one-second divisor. */
    ryz_host_display_advance_ms(2000);
    assert(ryz_lvgl_system_pump(NULL, NULL, NULL) == ESP_OK);
    assert(ryz_lvgl_get_fps(&fps) && fps == 0);
    assert(ryz_lvgl_system_release() == ESP_OK);
    assert(!ryz_lvgl_get_fps(&fps) && fps == 0);
    puts("PASS official cap, delayed sampling, released-owner rejection");
    return 0;
}
