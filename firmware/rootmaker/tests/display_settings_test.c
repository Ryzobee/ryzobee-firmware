#include "ryz_display_settings_internal.h"
#include "display.h"
#include "touch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ryz_display_settings_value_t stored,hardware;
static esp_err_t read_error=ESP_ERR_NOT_FOUND,write_error,apply_error,sleep_error;
static bool write_visible,blocked,asleep;
static unsigned reads,writes,applies,touch_sets,sleeps;
static unsigned brightness_sets;
static esp_err_t brightness_error;
static esp_err_t read_config(ryz_display_settings_value_t *out)
{ ++reads; *out=stored; return read_error; }
static esp_err_t write_config(ryz_display_settings_value_t value,bool *confirmed)
{
    ++writes; *confirmed=write_error==ESP_OK;
    if(*confirmed || write_visible) { stored=value; read_error=ESP_OK; }
    return write_error;
}
esp_err_t ryz_display_apply_configuration(uint8_t rotation,uint8_t brightness)
{
    ++applies;
    if(apply_error!=ESP_OK) return apply_error;
    hardware.rotation=rotation; hardware.brightness=brightness; blocked=false; return ESP_OK;
}
bool ryz_display_configuration_blocked(void) { return blocked; }
esp_err_t ryz_touch_set_rotation(uint8_t rotation)
{ ++touch_sets; assert(rotation==hardware.rotation); return ESP_OK; }
esp_err_t ryz_display_set_asleep(bool value)
{ ++sleeps; if(sleep_error!=ESP_OK) return sleep_error; asleep=value; return ESP_OK; }
esp_err_t ryz_display_set_brightness(uint8_t value)
{ ++brightness_sets; if(brightness_error!=ESP_OK) return brightness_error; hardware.brightness=value; blocked=false; return ESP_OK; }
static ryz_display_settings_snapshot_t snap(void)
{ ryz_display_settings_snapshot_t out; assert(ryz_display_settings_get_snapshot(&out)==ESP_OK); return out; }
static void init(void)
{
    const ryz_display_settings_storage_t backend={read_config,write_config};
    assert(ryz_display_settings_core_init(&backend)==ESP_OK);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_LOADING && !snap().ready && reads==0);
    assert(ryz_display_settings_core_init(&backend)==ESP_ERR_INVALID_STATE);
    ryz_display_settings_worker_tick();
    assert(reads==1 && applies==0 && !snap().ready);
    assert(ryz_display_settings_owner_tick(100,false,true));
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_READY && snap().ready && applies==1 && touch_sets==1);
    ryz_touch_sample_t up={.event=RYZ_TOUCH_UP};
    assert(ryz_display_settings_filter_touch(&up,ESP_OK,100));
    assert(!ryz_display_settings_filter_touch(&up,ESP_OK,101));
}
static void submit(ryz_display_settings_value_t value)
{
    uint64_t revision=snap().revision;
    assert(ryz_display_settings_submit(revision-1,value)==ESP_ERR_INVALID_STATE);
    assert(ryz_display_settings_submit(revision,value)==ESP_OK);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_SAVING);
    assert(ryz_display_settings_submit(snap().revision,value)==ESP_ERR_TIMEOUT);
}
static void transaction(void)
{
    init();
    assert(ryz_display_settings_equal(snap().confirmed,ryz_display_settings_defaults()) && writes==0);
    assert(hardware.rotation==0 && hardware.brightness==50);
    ryz_display_settings_value_t next={3,0,80};
    submit(next);
    assert(!snap().persisted && snap().confirmed.rotation==0);
    ryz_display_settings_worker_tick();
    assert(writes==1 && applies==1 && ryz_display_settings_equal(stored,next));
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_SAVING && snap().confirmed.rotation==0);
    ryz_display_settings_owner_tick(200,false,false);
    assert(applies==1 && snap().phase==RYZ_DISPLAY_SETTINGS_SAVING && writes==1);
    assert(ryz_display_settings_owner_tick(300,true,true));
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_SAVED && snap().persisted && snap().error==ESP_OK);
    assert(ryz_display_settings_equal(snap().confirmed,next) && hardware.rotation==3 && hardware.brightness==80);
    /* No request means no writes, regardless of redraw/worker ticks. */
    for(unsigned i=0;i<100;++i) ryz_display_settings_worker_tick();
    assert(writes==1);
}
static void unknown(void)
{
    init(); ryz_display_settings_value_t next={1,2,75};
    write_error=ESP_ERR_TIMEOUT; write_visible=true;
    submit(next); ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(200,true,true);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_UNKNOWN && !snap().persisted && applies==1 && writes==1);
    /* UNKNOWN explicit retry observes the whole saved tuple, no second write. */
    submit(next); ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(300,true,true);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_SAVED && writes==1 && applies==2);
    next.rotation=2; read_error=ESP_ERR_TIMEOUT;
    submit(next); ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(400,true,true);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_UNKNOWN && writes==1 && applies==2);
}
static void apply_failure(void)
{
    init(); ryz_display_settings_value_t next={2,1,65};
    apply_error=ESP_ERR_TIMEOUT;
    submit(next); ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(200,true,true);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_APPLY_FAILED && snap().persisted);
    assert(snap().confirmed.rotation==0 && touch_sets==1 && !snap().input_blocked);
    ryz_display_settings_owner_tick(5000,true,true);
    assert(applies==2 && writes==1); /* Safe rollback exposes explicit retry. */
    apply_error=ESP_OK;
    submit(next); ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(5100,true,true);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_SAVED && writes==1 && touch_sets==2);
    blocked=true; apply_error=ESP_ERR_TIMEOUT; next.rotation=3;
    submit(next); ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(6000,true,true);
    ryz_touch_sample_t down={.pressed=true,.has_position=true,.event=RYZ_TOUCH_DOWN};
    assert(snap().input_blocked && ryz_display_settings_filter_touch(&down,ESP_OK,6000));
    unsigned count=applies;
    for(unsigned i=1;i<=5;++i) ryz_display_settings_owner_tick(6000+i*1000,true,true);
    assert(applies==count+3 && writes==2 && snap().phase==RYZ_DISPLAY_SETTINGS_APPLY_FAILED);
}
static void idle_sleep(void)
{
    stored=(ryz_display_settings_value_t){1,0,85}; read_error=ESP_OK; init();
    assert(snap().persisted && hardware.rotation==1 && hardware.brightness==85);
    ryz_display_settings_owner_tick(100000,false,true); assert(!asleep); /* Boot/fault suppression. */
    ryz_display_settings_owner_tick(114999,true,true); assert(!asleep);
    ryz_display_settings_owner_tick(115000,true,true); assert(asleep && snap().asleep && sleeps==1);
    ryz_touch_sample_t down={.pressed=true,.has_position=true,.event=RYZ_TOUCH_DOWN};
    ryz_touch_sample_t up={.event=RYZ_TOUCH_UP};
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,116000) && !asleep && !snap().asleep);
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,116050));
    assert(!ryz_display_settings_filter_touch(NULL,ESP_ERR_TIMEOUT,116100));
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,116150));
    assert(ryz_display_settings_filter_touch(&up,ESP_OK,116200));
    assert(!ryz_display_settings_filter_touch(&down,ESP_OK,116250));
    ryz_display_settings_owner_tick(131249,true,true); assert(!asleep);
    ryz_display_settings_owner_tick(131250,true,true); assert(asleep);
    sleep_error=ESP_ERR_INVALID_STATE; /* privacy barrier must not be bypassed. */
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,132000) && asleep);
    sleep_error=ESP_OK;
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,132050) && !asleep);
    assert(ryz_display_settings_filter_touch(&up,ESP_OK,132100));
}
static void uncertain_sleep(void)
{
    stored=(ryz_display_settings_value_t){0,0,50}; read_error=ESP_OK; init();
    sleep_error=ESP_ERR_TIMEOUT;
    ryz_display_settings_owner_tick(15100,true,true);
    assert(!asleep && !snap().asleep && snap().input_blocked && sleeps==1);
    ryz_touch_sample_t down={.pressed=true,.has_position=true,.event=RYZ_TOUCH_DOWN};
    ryz_touch_sample_t up={.event=RYZ_TOUCH_UP};
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,15200) && sleeps==2);
    assert(ryz_display_settings_filter_touch(&up,ESP_OK,15210));
    assert(snap().input_blocked);
    sleep_error=ESP_OK;
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,15300) && sleeps==3);
    assert(!asleep && !snap().input_blocked);
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,15310));
    assert(ryz_display_settings_filter_touch(&up,ESP_OK,15320));
    assert(!ryz_display_settings_filter_touch(&down,ESP_OK,15330));
}
static void preview(void)
{
    init();
    uint64_t revision=snap().revision;
    assert(ryz_display_settings_preview(revision-1,80)==ESP_ERR_INVALID_STATE);
    assert(ryz_display_settings_preview(revision,53)==ESP_ERR_INVALID_ARG);
    assert(ryz_display_settings_preview(revision,80)==ESP_OK);
    assert(ryz_display_settings_preview(revision,95)==ESP_OK);
    assert(hardware.brightness==50 && brightness_sets==0); /* Intent is copy-only. */
    assert(!ryz_display_settings_owner_tick(200,true,true));
    assert(hardware.brightness==95 && brightness_sets==1 && snap().revision==revision);
    assert(snap().confirmed.brightness==50 && snap().requested.brightness==50);
    assert(reads==1 && writes==0 && applies==1 && touch_sets==1);
    ryz_display_settings_end_preview();
    ryz_display_settings_owner_tick(210,true,true);
    assert(hardware.brightness==50 && brightness_sets==2 && writes==0);
    ryz_display_settings_owner_tick(220,true,true);
    assert(brightness_sets==2); /* No permanent reapplication each frame. */
}
static void preview_save(void)
{
    init();
    assert(ryz_display_settings_preview(snap().revision,95)==ESP_OK);
    ryz_display_settings_owner_tick(200,true,true);
    ryz_display_settings_value_t next={1,3,95};
    submit(next);
    assert(ryz_display_settings_preview(snap().revision,20)==ESP_ERR_TIMEOUT);
    ryz_display_settings_end_preview(); /* Forced route exit while saving. */
    ryz_display_settings_owner_tick(210,true,true);
    assert(hardware.brightness==95 && brightness_sets==1);
    ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(220,true,true);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_SAVED && snap().confirmed.brightness==95 && writes==1);
    ryz_display_settings_end_preview(); ryz_display_settings_owner_tick(230,true,true);
    assert(hardware.brightness==95 && brightness_sets==1); /* Never restore old 50%. */
    assert(ryz_display_settings_preview(snap().revision,25)==ESP_OK);
    ryz_display_settings_owner_tick(240,true,true);
    write_error=ESP_ERR_TIMEOUT;
    next.brightness=25; submit(next);
    ryz_display_settings_worker_tick(); ryz_display_settings_owner_tick(250,true,true);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_UNKNOWN && hardware.brightness==25);
    assert(snap().confirmed.brightness==95 && writes==2);
    ryz_display_settings_end_preview(); ryz_display_settings_owner_tick(260,true,true);
    assert(hardware.brightness==95 && writes==2 && snap().preview_error==ESP_OK);
}
static void preview_deferred(void)
{
    stored=(ryz_display_settings_value_t){2,0,50}; read_error=ESP_OK; init();
    assert(ryz_display_settings_preview(snap().revision,10)==ESP_OK);
    ryz_display_settings_owner_tick(200,true,false);
    assert(hardware.brightness==50 && brightness_sets==0);
    ryz_display_settings_owner_tick(210,true,true);
    assert(hardware.brightness==10 && hardware.rotation==2);
    ryz_display_settings_owner_tick(15100,true,true);
    assert(asleep);
    ryz_display_settings_end_preview();
    ryz_display_settings_owner_tick(15200,true,false);
    assert(hardware.brightness==10 && asleep);
    ryz_display_settings_owner_tick(15210,true,true);
    assert(hardware.brightness==50 && asleep && writes==0 && touch_sets==1);
    ryz_touch_sample_t down={.event=RYZ_TOUCH_DOWN,.pressed=true,.has_position=true};
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,15300) && !asleep);
    assert(hardware.brightness==50);
}
static void preview_failures(void)
{
    init();
    brightness_error=ESP_ERR_TIMEOUT;
    assert(ryz_display_settings_preview(snap().revision,80)==ESP_OK);
    ryz_display_settings_owner_tick(200,true,true);
    assert(snap().preview_error==ESP_ERR_TIMEOUT && !snap().input_blocked && hardware.brightness==50);
    assert(snap().phase==RYZ_DISPLAY_SETTINGS_READY && writes==0);
    brightness_error=ESP_OK;
    assert(ryz_display_settings_preview(snap().revision,80)==ESP_OK);
    ryz_display_settings_owner_tick(210,true,true);
    assert(hardware.brightness==80 && snap().preview_error==ESP_OK);
    brightness_error=ESP_ERR_TIMEOUT;
    ryz_display_settings_end_preview(); ryz_display_settings_owner_tick(220,true,true);
    assert(snap().input_blocked && hardware.brightness==80 && writes==0);
    unsigned count=brightness_sets;
    ryz_display_settings_owner_tick(1219,true,true); assert(brightness_sets==count);
    brightness_error=ESP_OK;
    ryz_display_settings_owner_tick(1220,true,true);
    assert(hardware.brightness==50 && !snap().input_blocked && snap().preview_error==ESP_OK && writes==0);
    blocked=true; brightness_error=ESP_ERR_TIMEOUT;
    assert(ryz_display_settings_preview(snap().revision,100)==ESP_OK);
    ryz_display_settings_owner_tick(1230,true,true);
    assert(snap().input_blocked);
    ryz_touch_sample_t down={.event=RYZ_TOUCH_DOWN,.pressed=true,.has_position=true};
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,1240));
    brightness_error=ESP_OK;
    ryz_display_settings_owner_tick(2230,true,true);
    assert(hardware.brightness==100 && !snap().input_blocked && writes==0);
    /* Input held during a failed PWM update cannot activate a button later. */
    assert(ryz_display_settings_filter_touch(&down,ESP_OK,2240));
    ryz_touch_sample_t up={.event=RYZ_TOUCH_UP};
    assert(ryz_display_settings_filter_touch(&up,ESP_OK,2250));
    assert(!ryz_display_settings_filter_touch(&down,ESP_OK,2260));
}
int main(int argc,char **argv)
{
    assert(argc==2);
    assert(ryz_display_settings_valid(ryz_display_settings_defaults()));
    assert(!ryz_display_settings_valid((ryz_display_settings_value_t){4,0,50}));
    assert(!ryz_display_settings_valid((ryz_display_settings_value_t){0,5,50}));
    assert(!ryz_display_settings_valid((ryz_display_settings_value_t){0,4,53}));
    assert(ryz_display_settings_timeout_ms(0)==15000 && ryz_display_settings_timeout_ms(4)==0);
    if(!strcmp(argv[1],"transaction")) transaction();
    else if(!strcmp(argv[1],"preview")) preview();
    else if(!strcmp(argv[1],"preview-save")) preview_save();
    else if(!strcmp(argv[1],"preview-deferred")) preview_deferred();
    else if(!strcmp(argv[1],"preview-failures")) preview_failures();
    else if(!strcmp(argv[1],"unknown")) unknown();
    else if(!strcmp(argv[1],"apply-failure")) apply_failure();
    else if(!strcmp(argv[1],"sleep")) idle_sleep();
    else if(!strcmp(argv[1],"sleep-uncertain")) uncertain_sleep();
    else if(!strcmp(argv[1],"corrupt")) { read_error=ESP_ERR_INVALID_ARG; init(); assert(snap().load_error==ESP_ERR_INVALID_ARG && writes==0); }
    else if(!strcmp(argv[1],"load-error")) { read_error=ESP_ERR_TIMEOUT; init(); assert(snap().load_error==ESP_ERR_TIMEOUT && writes==0); }
    else assert(false);
    printf("DISPLAY_SETTINGS_PASS %s\n",argv[1]); return 0;
}
