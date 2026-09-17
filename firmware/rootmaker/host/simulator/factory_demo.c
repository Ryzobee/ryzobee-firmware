/* Real Lua/public APIs/LVGL. Virtual bytes and sensor samples are explicitly
 * synthetic; scan/history/test decisions are owned only by the Lua scripts. */
#include "factory_demo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "ryz_font.h"
#include "ryz_lvgl.h"
#include "../pixels/monitor_stream_fixture.h"
#include "monitor_source.h"
#include "i2c_source.h"
#include "hardware_source.h"

static const ryz_sim_factory_source_t sources[] = {
    {RYZ_SIM_FACTORY_MONITOR,"tool_monitor.lua",monitor_source,monitor_version,monitor_sha256,sizeof(monitor_source)},
    {RYZ_SIM_FACTORY_I2C,"tool_i2c.lua",i2c_source,i2c_version,i2c_sha256,sizeof(i2c_source)},
    {RYZ_SIM_FACTORY_HARDWARE,"tool_hardware.lua",hardware_source,hardware_version,hardware_sha256,sizeof(hardware_source)},
};
#define INPUT_CAPACITY 16U
static struct {
    bool active,cancelled,pointer_pressed,imu_initialized;
    ryz_sim_factory_kind_t kind;
    void (*idle)(unsigned);
    void (*present)(void);
    ryz_ui_scene_t scene;
    ryz_app_pointer_t input[INPUT_CAPACITY];
    unsigned head,count,produced,populated;
    uint64_t next_input;
    uint32_t identity,i2c_handle;
    bool i2c_board;
    struct {
        uint32_t handle;
        uint64_t ready_at;
        bool closing;
        ryz_peripheral_led_state_t state;
        uint8_t red,green,blue;
    } led;
    monitor_stream_fixture_t stream;
} demo;
static uint64_t now_ms(void *p) { (void)p; return (uint64_t)esp_timer_get_time()/1000U; }
const ryz_sim_factory_source_t *ryz_sim_factory_source(ryz_sim_factory_kind_t kind)
{
    for(unsigned i=0;i<sizeof(sources)/sizeof(*sources);++i) if(sources[i].kind==kind) return &sources[i];
    return NULL;
}
const ryz_sim_factory_source_t *ryz_sim_factory_find(const char *name)
{
    if(name) for(unsigned i=0;i<sizeof(sources)/sizeof(*sources);++i)
        if(!strcmp(sources[i].name,name)) return &sources[i];
    return NULL;
}
ryz_sim_factory_kind_t ryz_sim_factory_kind(void) { return demo.kind; }
bool ryz_sim_factory_active(void) { return demo.active; }
const ryz_ui_scene_t *ryz_sim_factory_scene(void) { return &demo.scene; }
void ryz_sim_factory_cancel(void) { if(demo.active) demo.cancelled=true; }
void ryz_sim_factory_pointer(const ryz_app_pointer_t *sample)
{
    if(!demo.active || (sample->phase==RYZ_APP_POINTER_NONE && !sample->interrupted)) return;
    if(sample->interrupted || demo.count==INPUT_CAPACITY) {
        demo.head=0; demo.count=1; demo.pointer_pressed=false;
        demo.input[0]=(ryz_app_pointer_t){.interrupted=true,.sampled_ms=(uint32_t)now_ms(NULL)};
        return;
    }
    if(sample->phase==RYZ_APP_POINTER_MOVE && demo.count) {
        unsigned last=(demo.head+demo.count-1U)%INPUT_CAPACITY;
        if(demo.input[last].phase==RYZ_APP_POINTER_MOVE) { demo.input[last]=*sample; return; }
    }
    demo.input[(demo.head+demo.count)%INPUT_CAPACITY]=*sample; ++demo.count;
}
static int pointer_read(void *p,ryz_app_pointer_t *out)
{
    (void)p; *out=(ryz_app_pointer_t){.pressed=demo.pointer_pressed,.sampled_ms=(uint32_t)now_ms(NULL)};
    if(demo.count) {
        *out=demo.input[demo.head]; demo.head=(demo.head+1U)%INPUT_CAPACITY; --demo.count;
        demo.pointer_pressed=out->pressed;
    }
    return ESP_OK;
}
static void append(const char *text) { monitor_fixture_feed(&demo.stream,text,strlen(text)); }
static void tick(void)
{
    uint64_t now=now_ms(NULL);
    if(demo.led.handle && demo.led.state==RYZ_PERIPHERAL_LED_PENDING && now>=demo.led.ready_at)
        demo.led.state=RYZ_PERIPHERAL_LED_READY;
    if(!demo.stream.handle) return;
    if(demo.populated!=demo.stream.opened) {
        demo.populated=demo.stream.opened; demo.next_input=now+1000U;
        append("SIMULATED INPUT ONLY\nNO DEVICE CONNECTED\nPAUSE / CLEAR / CONFIG\n");
        for(unsigned i=1;i<=8;++i) {
            char line[32]; snprintf(line,sizeof(line),"SIM SAMPLE / %02u\n",i); append(line);
        }
    }
    if(now>=demo.next_input) {
        char line[48]; snprintf(line,sizeof(line),"SIM %s / %06u\n",
            demo.stream.kind==RYZ_PERIPHERAL_UART ? "UART1" : "SYSTEM",++demo.produced);
        append(line); demo.next_input=now+1000U;
    }
}
static void pause_ms(void *p,unsigned ms) { (void)p; demo.idle(ms); tick(); }
static bool cancelled(void *p) { (void)p; return demo.cancelled; }
static void *resize(void *p,size_t n) { if(!n) { free(p); return NULL; } return realloc(p,n); }
static int mount(void *p,const ryz_ui_scene_t *s)
{
    (void)p; int error=ryz_lvgl_mount(s);
    if(!error) {
        demo.scene=*s; demo.head=demo.count=0; demo.pointer_pressed=false;
        demo.present();
    }
    return error;
}
static int update(void *p,const ryz_ui_scene_t *s)
{ (void)p; int error=ryz_lvgl_update(s); if(!error) { demo.scene=*s; demo.present(); } return error; }
static int pump(void *p) { (void)p; int error=ryz_lvgl_pump(); demo.present(); return error; }
static void close_ui(void *p) { (void)p; ryz_lvgl_release(); demo.scene=(ryz_ui_scene_t){0}; }
static void cleanup(void *p)
{
    (void)p;
    /* Generic Lua cleanup closes all bus/stream handles. Virtual sensor/RGB
     * teardown has no external pending work: cancel samples and set RGB off. */
    assert(!demo.stream.handle && !demo.i2c_handle);
    demo.imu_initialized=false;
    demo.led.handle=0; demo.led.closing=false;
    demo.led.red=demo.led.green=demo.led.blue=0;
    demo.led.state=RYZ_PERIPHERAL_LED_IDLE;
}
static ryz_peripheral_result_t peripheral(void *p,const ryz_peripheral_request_t *q,
    ryz_peripheral_reply_t *out)
{
    (void)p; memset(out,0,sizeof(*out));
    if(q->kind==RYZ_PERIPHERAL_LOG || q->kind==RYZ_PERIPHERAL_UART)
        return monitor_fixture_call(&demo.stream,q,out);
    if(q->kind==RYZ_PERIPHERAL_I2C) {
        if(q->op==RYZ_PERIPHERAL_OPEN) {
            if(demo.i2c_handle) return RYZ_PERIPHERAL_BUSY;
            demo.i2c_board=q->config.i2c.board;
            out->handle=demo.i2c_handle=++demo.identity;
            return RYZ_PERIPHERAL_OK;
        }
        if(!demo.i2c_handle || q->handle!=demo.i2c_handle) return RYZ_PERIPHERAL_CLOSED;
        if(q->op==RYZ_PERIPHERAL_CLOSE) { demo.i2c_handle=0; return RYZ_PERIPHERAL_OK; }
        if(q->op!=RYZ_PERIPHERAL_PROBE) return RYZ_PERIPHERAL_UNSUPPORTED;
        /* Declared virtual ACK peers, not identification of physical chips. */
        bool ack=q->address==0x15 || (!demo.i2c_board && (q->address==0x50 || q->address==0x68));
        return ack ? RYZ_PERIPHERAL_OK : RYZ_PERIPHERAL_NOT_FOUND;
    }
    if(q->kind!=RYZ_PERIPHERAL_LED) return RYZ_PERIPHERAL_UNAVAILABLE;
    if(q->op==RYZ_PERIPHERAL_OPEN) {
        if(demo.led.handle) return RYZ_PERIPHERAL_BUSY;
        memset(&demo.led,0,sizeof(demo.led)); out->handle=demo.led.handle=++demo.identity;
        return RYZ_PERIPHERAL_OK;
    }
    if(!demo.led.handle || q->handle!=demo.led.handle) return RYZ_PERIPHERAL_CLOSED;
    if(q->op==RYZ_PERIPHERAL_CLOSE) {
        if(!demo.led.closing && (demo.led.red || demo.led.green || demo.led.blue)) {
            demo.led.closing=true; demo.led.red=demo.led.green=demo.led.blue=0;
            demo.led.state=RYZ_PERIPHERAL_LED_PENDING; demo.led.ready_at=now_ms(NULL)+60;
        }
        if(demo.led.state==RYZ_PERIPHERAL_LED_PENDING) return RYZ_PERIPHERAL_BUSY;
        demo.led.handle=0; return RYZ_PERIPHERAL_OK;
    }
    if(demo.led.closing) return RYZ_PERIPHERAL_CLOSED;
    if(q->op==RYZ_PERIPHERAL_WRITE) {
        if(demo.led.state==RYZ_PERIPHERAL_LED_PENDING) return RYZ_PERIPHERAL_BUSY;
        assert(q->length==3 && q->data);
        demo.led.red=q->data[0]; demo.led.green=q->data[1]; demo.led.blue=q->data[2];
        demo.led.state=RYZ_PERIPHERAL_LED_PENDING; demo.led.ready_at=now_ms(NULL)+60;
        printf("SIM RGB intent: %u/%u/%u; no physical LED\n",demo.led.red,demo.led.green,demo.led.blue);
        return RYZ_PERIPHERAL_OK;
    }
    if(q->op!=RYZ_PERIPHERAL_STATUS) return RYZ_PERIPHERAL_UNSUPPORTED;
    out->led.state=demo.led.state;
    out->led.red=demo.led.red; out->led.green=demo.led.green; out->led.blue=demo.led.blue;
    out->led.output_known=demo.led.state==RYZ_PERIPHERAL_LED_READY;
    return RYZ_PERIPHERAL_OK;
}
static ryz_lua_hardware_result_t hardware(void *p,ryz_lua_hardware_op_t op,
    ryz_lua_hardware_value_t *out)
{
    (void)p; memset(out,0,sizeof(*out));
    if(op==RYZ_LUA_IMU_INIT) { demo.imu_initialized=true; return RYZ_LUA_HW_OK; }
    if(op==RYZ_LUA_IMU_DEINIT) { demo.imu_initialized=false; return RYZ_LUA_HW_OK; }
    if(op!=RYZ_LUA_IMU_READ) return RYZ_LUA_HW_UNAVAILABLE;
    if(!demo.imu_initialized) return RYZ_LUA_HW_NOT_INITIALIZED;
    uint32_t step=(uint32_t)(now_ms(NULL)/100U);
    out->sequence=step+1U; out->timestamp_us=(uint64_t)step*100000U;
    out->x_mg=((int)(step%20U)-10)*65.0f;
    out->y_mg=-out->x_mg/2.0f; out->z_mg=900.0f+(step%7U)*50.0f;
    return RYZ_LUA_HW_OK;
}
void ryz_sim_factory_run(ryz_sim_factory_kind_t kind,void (*idle)(unsigned),
    void (*present)(void),ryz_lua_result_t *result)
{
    const ryz_sim_factory_source_t *source=ryz_sim_factory_source(kind);
    assert(!demo.active && idle && present && result && source);
    memset(&demo,0,sizeof(demo));
    demo.active=true; demo.kind=kind; demo.idle=idle; demo.present=present; demo.identity=1;
    monitor_fixture_init(&demo.stream);
    assert(ryz_font_init()==ESP_OK);
    const ryz_app_platform_t platform={.resize=resize,.now_ms=now_ms,.pause_ms=pause_ms,
        .cancelled=cancelled,.pointer_read=pointer_read,.ui_mount=mount,.ui_update=update,
        .ui_pump=pump,.ui_close=close_ui,.cleanup=cleanup,.peripheral_call=peripheral,
        .hardware_call=hardware,.tool_call=NULL,.seed=1};
    char name[64]; snprintf(name,sizeof(name),"@%s",source->name);
    ryz_app_execute(source->source,source->bytes,name,0,&platform,result);
    cleanup(NULL); close_ui(NULL); demo.active=false;
    printf("FACTORY DEMO %s v%s sha256=%s: phase=%s bytes=%zu peak=%zu; SIMULATED, NO DEVICE ACCESS\n",
        source->name,source->version,source->sha256,result->phase,source->bytes,result->peak_bytes);
    if(!result->ok && strcmp(result->phase,"stopped")) fprintf(stderr,"Lua error: %s\n",result->error);
}
