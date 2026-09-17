#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "workbench_fault.h"
#include "ryz_v5_render.h"
#include "ryz_v5_widgets.h"
#include "ryz_display_host.h"
#include "ryz_pixels_host.h"

static ryz_system_ui_snapshot_t system_view={.ap_active=true,
    .ap_ssid="DEMO-AP",.ap_password="DEMO;pass:123",.portal_ip="192.168.4.1"};

static bool label(lv_obj_t *root,const char *text)
{
    if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return true;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i)
        if(label(lv_obj_get_child(root,(int32_t)i),text)) return true;
    return false;
}

static void mapping(void)
{
    ryz_workbench_fault_t f;
    ryz_lua_result_t r={.phase="runtime",.elapsed_ms=271,.peak_bytes=4096,
        .error="[string \"demo.lua\"]:27: error DEMO;pass:123"};
    const char *out="first\nDEMO;pass:123\nlast";
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1.2.3",1,out,strlen(out),false,system_view.ap_password));
    assert(!strcmp(f.screen.code,"LUA-R01") && f.screen.line==27);
    assert(!strstr(f.report.error,system_view.ap_password) && !strstr(f.report.output,system_view.ap_password));
    assert(!f.report.lua_ok && !f.report.ok && f.report.cleanup_ok==1 && f.report.duration_ms==271);
    char split[513]; memset(split,'A',sizeof(split));
    memcpy(split,system_view.ap_password,strlen(system_view.ap_password));
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,split,sizeof(split),false,system_view.ap_password));
    for(size_t i=0;i<strlen(system_view.ap_password)-1;++i) assert(f.report.output[i]=='*');
    memset(r.error,'X',sizeof(r.error));
    memcpy(r.error+250,system_view.ap_password,5); r.error[255]=0;
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,"",0,false,system_view.ap_password));
    assert(!strcmp(f.report.error+250,"*****") && f.report.error_truncated);
    strcpy(r.error,"foreign.lua:53: error 120");
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,"",0,false,NULL) && f.screen.line==-1);
    strcpy(r.error,"demo.lua:2147483648: overflow");
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,"",0,false,NULL) && f.screen.line==-1);
    strcpy(r.error,"demo.lua:12x: malformed");
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,"",0,false,NULL) && f.screen.line==-1);
    char long_output[900]; memset(long_output,'A',sizeof(long_output));
    r.phase="syntax";
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,long_output,sizeof(long_output),false,NULL));
    assert(f.report.output_truncated && strlen(f.report.output)==512 && !strcmp(f.screen.code,"LUA-S01"));
    r.phase="timeout";
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,"",0,false,NULL) && !strcmp(f.screen.code,"LUA-T01"));
    r.phase="stopped";
    assert(!ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",0,"",0,false,NULL));
    r.ok=true; r.phase="done";
    assert(!ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",1,"",0,false,NULL));
    assert(ryz_workbench_fault_prepare(&f,&r,"demo.lua","J1","1",-1,"",0,false,NULL));
    assert(f.report.lua_ok && !f.report.ok && !strcmp(f.report.phase,"cleanup") && !strcmp(f.screen.code,"CLEANUP"));
    puts("PASS fault phases, reliable source line, bounded tail, known secret redaction and stop/success exclusion");
}

static void render(const char *directory,const char *name)
{
    assert(ryz_v5_render(RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT,&system_view,NULL,NULL,NULL)==ESP_OK);
    assert(!label(lv_screen_active(),"RYZOBEE") && !label(lv_screen_active(),"APPS"));
    if(directory) ryz_host_display_save(directory,name);
}

static void qr_quiet(void)
{
    unsigned found=0;
    lv_obj_t *root=lv_screen_active();
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *o=lv_obj_get_child(root,(int32_t)i);
        if(!lv_obj_check_type(o,&lv_image_class)) continue;
        const lv_image_dsc_t *image=lv_image_get_src(o);
        assert(image->header.w==123 && image->header.h==123 && image->header.cf==LV_COLOR_FORMAT_A8);
        assert(lv_obj_get_x(o)==8 && lv_obj_get_y(o)==66);
        for(unsigned y=0;y<123;++y) for(unsigned x=0;x<123;++x)
            if(x<8 || x>=115 || y<8 || y>=115) assert(image->data[y*123+x]==0);
        ++found;
    }
    assert(found==1);
}

int main(int argc,char **argv)
{
    mapping();
    const char *directory=argc>1?argv[1]:NULL;
    ryz_host_display_reset();
    ryz_v5_fault_model_t f={.code="LUA-R01",.summary="Runtime error",.source_file="tool_monitor.lua",.line=42};
    ryz_v5_fault_set(&f);
    render(directory,"v5-fault-unavailable-240");
    assert(label(lv_screen_active(),"LINK UNAVAILABLE") && ryz_v5_fault_hit(12,200)==1);
    assert(ryz_v5_fault_link("/f/0123abcd?cap=0123456789abcdef0123456789abcdef"));
    render(directory,"v5-fault-ap-join-240"); qr_quiet();
    assert(ryz_v5_fault_hit(115,196)==1 && ryz_v5_fault_hit(120,196)==0 && ryz_v5_fault_hit(124,196)==2);
    ryz_v5_fault_next();
    render(directory,"v5-fault-ap-log-240"); qr_quiet();
    assert(label(lv_screen_active(),"tool_monitor.lua") && label(lv_screen_active(),"LINE 42"));
    assert(ryz_v5_fault_hit(115,196)==2 && ryz_v5_fault_hit(124,196)==1);
    assert(ryz_v5_fault_link(NULL));
    render(directory,"v5-fault-expired-240");
    assert(label(lv_screen_active(),"LINK UNAVAILABLE") && ryz_v5_fault_hit(12,200)==1);
    assert(ryz_v5_release()==ESP_OK);
    assert(!ryz_v5_fault_needs_cleanup());
    puts("PASS native fault states, QR integer raster/transparent quiet zone, fixed actions and unavailable gate");
    return 0;
}
