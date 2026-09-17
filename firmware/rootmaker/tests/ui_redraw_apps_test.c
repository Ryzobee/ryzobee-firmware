#define main original_apps_main
#include "v5_apps_ui_test.c"
#undef main
#define RYZ_REDRAW_FULL_ORACLE
static void audit_full_render(void);
#include "ui_redraw_measure.h"
static void audit_full_render(void)
{
    assert(ryz_v5_release()==ESP_OK);
    (void)render_full();
}
static void paint_tick(uint64_t now)
{ if(ryz_v5_apps_tick(now)) (void)render_full(); }
int main(int argc,char **argv)
{
    (void)argc; (void)argv; catalog(); (void)render_full();
    audit_begin(); down(120,100,100); paint_tick(100); audit_end("APPS row DOWN before debounce");
    audit_begin(); paint_tick(180); audit_end("APPS row stable highlight");
    audit_begin(); move(120,85,200); paint_tick(200); audit_end("APPS list scroll 15px");
    audit_begin(); move(120,80,220); paint_tick(220); audit_end("APPS list scroll 5px");
    up(240); paint_tick(240);
    detail("info.lua",9); paint_tick(300);
    down(120,130,400); paint_tick(400);
    audit_begin(); move(120,110,420); paint_tick(420); audit_end("APP INFO scroll 20px");
    up(450); paint_tick(450);
    audit_begin(); down(120,218,500); paint_tick(500); audit_end("APP INFO RUN already optimized");
    up(600); paint_tick(600);
    assert(ryz_v5_release()==ESP_OK);
    return audit_global ? 1 : 0;
}
