#define main original_display_main
#include "v5_display_ui_test.c"
#undef main
#define RYZ_REDRAW_FULL_ORACLE
static void audit_full_render(void);
#include "ui_redraw_measure.h"
static void audit_full_render(void)
{
    assert(ryz_v5_release()==ESP_OK);
    (void)draw(NULL,NULL);
}
static void audit_pointer(ryz_touch_event_t event,unsigned x,unsigned y)
{
    const ryz_touch_sample_t input={.event=event,.pressed=event==RYZ_TOUCH_DOWN || event==RYZ_TOUCH_MOVE,
        .has_position=event==RYZ_TOUCH_DOWN || event==RYZ_TOUCH_MOVE,.x=x,.y=y};
    if(ryz_v5_display_pointer(&input,&back)) (void)draw(NULL,NULL);
}
int main(int argc,char **argv)
{
    (void)argc; (void)argv; fixture(); (void)draw(NULL,NULL);
    audit_begin(); audit_pointer(RYZ_TOUCH_DOWN,120,171); audit_end("BRIGHTNESS 50->55 percent");
    audit_begin(); audit_pointer(RYZ_TOUCH_MOVE,121,171); audit_end("BRIGHTNESS same rounded value");
    audit_begin(); audit_pointer(RYZ_TOUCH_MOVE,135,171); audit_end("BRIGHTNESS 55->60 percent");
    audit_pointer(RYZ_TOUCH_UP,135,171);
    audit_begin(); audit_pointer(RYZ_TOUCH_DOWN,16,75); audit_pointer(RYZ_TOUCH_UP,16,75);
    audit_end("ORIENTATION already selected");
    audit_begin(); audit_pointer(RYZ_TOUCH_DOWN,90,75); audit_pointer(RYZ_TOUCH_UP,90,75);
    audit_end("ORIENTATION select new angle");
    assert(ryz_v5_release()==ESP_OK);
    return audit_global ? 1 : 0;
}
