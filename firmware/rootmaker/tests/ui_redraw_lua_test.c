#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ryz_lvgl.h"
#include "ryz_font.h"
#include "ryz_display_host.h"
#include "ui_redraw_measure.h"
int main(int argc,char **argv)
{
    (void)argc; (void)argv;
    ryz_ui_scene_t scene={.id="audit",.generation=1,.background=0,.object_count=1};
    scene.objects[0]=(ryz_ui_object_t){.id="value",.kind=RYZ_UI_LABEL,.x=40,.y=80,
        .width=160,.height=40,.text="COUNT 1",.foreground=0xffff,
        .font=RYZ_UI_FONT_BODY_16,.visible=true,.enabled=true};
    assert(ryz_font_init()==ESP_OK);
    assert(ryz_lvgl_mount(&scene)==ESP_OK);
    audit_begin(); assert(ryz_lvgl_update_checked(&scene,NULL,NULL)==ESP_OK);
    /* Direct adapter probe: app_runtime.c filters this case before dispatch. */
    audit_end("Lua adapter identical scene");
    audit_begin(); strcpy(scene.objects[0].text,"COUNT 2");
    assert(ryz_lvgl_update_checked(&scene,NULL,NULL)==ESP_OK);
    audit_end("Lua adapter one digit");
    ryz_lvgl_release();
    return audit_global ? 1 : 0;
}

