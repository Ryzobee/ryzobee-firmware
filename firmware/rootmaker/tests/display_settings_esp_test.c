/* Compile the actual backend to exercise its private wire codec/readback,
 * never expose raw NVS functions as a public firmware API just for testing. */
#include "../components/ryz_display_settings/ryz_display_settings_esp.c"
#include <assert.h>
#include <stdio.h>

static uint8_t disk[12],staging[12];
static size_t disk_size;
static bool exists,staged_valid,commit_visible;
static unsigned opens,closes,sets,commits,tasks,initializes,panel_calls;
static esp_err_t open_error,get_error,set_error,commit_error,init_error;
static BaseType_t create_result=pdPASS;
esp_err_t nvs_flash_init(void) { ++initializes; return init_error; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out)
{
    assert(!strcmp(name,"ryz_display") && (mode==NVS_READONLY || mode==NVS_READWRITE));
    if(open_error) return open_error;
    ++opens; *out=opens; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *size)
{
    assert(handle && !strcmp(key,"config") && size);
    if(get_error) return get_error;
    if(!exists) return ESP_ERR_NVS_NOT_FOUND;
    if(*size<disk_size) { *size=disk_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    *size=disk_size; memcpy(out,disk,disk_size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *data,size_t size)
{
    assert(handle && !strcmp(key,"config") && size==12); ++sets;
    if(set_error) return set_error;
    memcpy(staging,data,size); staged_valid=true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle); ++commits;
    if(staged_valid && (!commit_error || commit_visible)) {
        memcpy(disk,staging,12); exists=true; disk_size=12; staged_valid=false;
    }
    return commit_error;
}
void nvs_close(nvs_handle_t handle) { assert(handle); ++closes; }
BaseType_t xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,
                       void *context,unsigned priority,TaskHandle_t *out)
{
    assert(entry && !strcmp(name,"display_cfg") && stack==4096 && !context && priority==2);
    ++tasks; if(create_result==pdPASS) *out=(void *)1; return create_result;
}
void vTaskDelay(TickType_t delay) { (void)delay; assert(false); }
esp_err_t ryz_display_apply_configuration(uint8_t rotation,uint8_t brightness)
{ (void)rotation; (void)brightness; ++panel_calls; return ESP_OK; }
esp_err_t ryz_display_set_brightness(uint8_t brightness)
{ (void)brightness; ++panel_calls; return ESP_OK; }
bool ryz_display_configuration_blocked(void) { return false; }
esp_err_t ryz_touch_set_rotation(uint8_t rotation) { (void)rotation; ++panel_calls; return ESP_OK; }
esp_err_t ryz_display_set_asleep(bool asleep) { (void)asleep; ++panel_calls; return ESP_OK; }

static void boot_load(void)
{
    /* Bootstrap reads through the public boundary before any service worker or
     * panel exists. Missing preferences must keep the existing defaults. */
    ryz_display_settings_value_t actual={3,0,95};
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_NOT_FOUND);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    assert(initializes==1 && sets==0 && commits==0 && tasks==0 && opens==closes);
    assert(ryz_display_settings_load_boot(NULL)==ESP_ERR_INVALID_ARG && initializes==1);

    /* Fixed wire fixtures use independently computed standard CRC32 values.
     * Repeated loads intentionally see new tuples rather than cached state. */
    const uint8_t saved[4][12]={
        {'R','Y','Z','D',1,0,4,50,0xd0,0x0d,0xa1,0x4f},
        {'R','Y','Z','D',1,1,4,50,0xd1,0xcf,0xcb,0x78},
        {'R','Y','Z','D',1,2,4,50,0xd3,0x89,0x75,0x21},
        {'R','Y','Z','D',1,3,4,50,0xd2,0x4b,0x1f,0x16},
    };
    exists=true; disk_size=sizeof(disk);
    for(unsigned rotation=0;rotation<4;++rotation) {
        memcpy(disk,saved[rotation],sizeof(disk));
        assert(ryz_display_settings_load_boot(&actual)==ESP_OK);
        assert(actual.rotation==rotation && actual.sleep_index==4 && actual.brightness==50);
        assert(sets==0 && commits==0 && tasks==0 && panel_calls==0 && opens==closes);
    }
    disk[11]^=1;
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_INVALID_ARG);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    disk[11]^=1;
    /* Well-formed CRC is insufficient when the tuple itself is invalid. */
    const uint8_t invalid[12]={'R','Y','Z','D',1,4,4,50,0xd7,0x04,0x09,0x93};
    memcpy(disk,invalid,sizeof(disk));
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_INVALID_ARG);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    /* Valid independent CRC32 fixtures must still reject out-of-range and
     * non-step brightness rather than feeding unvalidated PWM into boot. */
    const uint8_t invalid_brightness[][12]={
        {'R','Y','Z','D',1,3,4,9,0x63,0x40,0xf6,0x32},
        {'R','Y','Z','D',1,3,4,101,0x27,0x44,0xdb,0x41},
        {'R','Y','Z','D',1,3,4,53,0x4c,0x2f,0x8a,0xb5},
    };
    for(unsigned i=0;i<sizeof(invalid_brightness)/sizeof(invalid_brightness[0]);++i) {
        memcpy(disk,invalid_brightness[i],sizeof(disk));
        assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_INVALID_ARG);
        assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
        assert(sets==0 && commits==0 && tasks==0 && panel_calls==0 && opens==closes);
    }
    memcpy(disk,saved[3],sizeof(disk));
    disk_size=11;
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_INVALID_ARG);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    disk_size=sizeof(disk);

    unsigned before=opens;
    actual=(ryz_display_settings_value_t){3,0,95}; init_error=ESP_ERR_NO_MEM;
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_NO_MEM && opens==before);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    init_error=ESP_OK; open_error=ESP_ERR_NVS_NOT_FOUND;
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_NOT_FOUND);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    open_error=ESP_ERR_TIMEOUT;
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_TIMEOUT);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    open_error=ESP_OK; get_error=ESP_ERR_TIMEOUT;
    assert(ryz_display_settings_load_boot(&actual)==ESP_ERR_TIMEOUT);
    assert(ryz_display_settings_equal(actual,ryz_display_settings_defaults()));
    get_error=ESP_OK;
    assert(sets==0 && commits==0 && tasks==0 && panel_calls==0 && opens==closes);
    ryz_display_settings_snapshot_t state;
    assert(ryz_display_settings_get_snapshot(&state)==ESP_ERR_INVALID_STATE);
    exists=false;
    puts("DISPLAY_SETTINGS_BOOT_PASS: read-only four orientations, missing/corrupt/invalid/init failures preserve defaults");
}

int main(void)
{
    boot_load();
    ryz_display_settings_value_t actual={0},desired={3,4,95}; bool confirmed=true;
    assert(read_settings(&actual)==ESP_ERR_NOT_FOUND && opens==closes);
    assert(write_settings(desired,&confirmed)==ESP_OK && confirmed && sets==1 && commits==1);
    assert(!memcmp(disk,"RYZD",4) && disk[4]==1 && disk[5]==3 && disk[6]==4 && disk[7]==95);
    assert(read_settings(&actual)==ESP_OK && ryz_display_settings_equal(actual,desired));
    /* SAVE uses the real committing backend; the next bootstrap observes
     * that exact non-default tuple without writing or touching the panel. */
    const unsigned saved_sets=sets,saved_commits=commits,saved_panel_calls=panel_calls;
    const unsigned saved_tasks=tasks,saved_initializes=initializes;
    actual=ryz_display_settings_defaults();
    assert(ryz_display_settings_load_boot(&actual)==ESP_OK);
    assert(ryz_display_settings_equal(actual,desired) && actual.brightness==95);
    assert(sets==saved_sets && commits==saved_commits && panel_calls==saved_panel_calls);
    assert(tasks==saved_tasks && initializes==saved_initializes+1 && opens==closes);
    puts("DISPLAY_SETTINGS_BRIGHTNESS_BOOT_PASS: committed 95 percent is loaded read-only before panel IO");
    for(unsigned i=0;i<12;++i) {
        disk[i]^=1; assert(read_settings(&actual)==ESP_ERR_INVALID_ARG); disk[i]^=1;
    }
    disk_size=11; assert(read_settings(&actual)==ESP_ERR_INVALID_ARG); disk_size=12;
    desired.rotation=1; commit_error=ESP_ERR_TIMEOUT; commit_visible=true;
    assert(write_settings(desired,&confirmed)==ESP_ERR_TIMEOUT && !confirmed);
    assert(read_settings(&actual)==ESP_OK && actual.rotation==1);
    commit_error=ESP_OK; get_error=ESP_ERR_TIMEOUT;
    assert(write_settings(desired,&confirmed)==ESP_ERR_TIMEOUT && !confirmed);
    get_error=ESP_OK; set_error=ESP_ERR_NO_MEM;
    unsigned before=commits;
    assert(write_settings(desired,&confirmed)==ESP_ERR_NO_MEM && !confirmed && commits==before);
    set_error=ESP_OK; open_error=ESP_ERR_NO_MEM;
    assert(write_settings(desired,&confirmed)==ESP_ERR_NO_MEM && !confirmed);
    open_error=ESP_OK; assert(opens==closes);
    create_result=0;
    assert(ryz_display_settings_start()==ESP_ERR_NO_MEM && tasks==1);
    create_result=pdPASS;
    assert(ryz_display_settings_start()==ESP_OK && tasks==2);
    assert(ryz_display_settings_start()==ESP_OK && tasks==2);
    puts("DISPLAY_SETTINGS_ESP_PASS: complete versioned CRC blob, fresh-handle readback, ambiguous commit, startup retry");
    return 0;
}
