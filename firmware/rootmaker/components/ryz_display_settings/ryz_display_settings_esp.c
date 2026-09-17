#include "ryz_display_settings_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static uint32_t checksum(const uint8_t *bytes,size_t length)
{
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<length;++i) {
        crc^=bytes[i];
        for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((0U-(crc&1U))&UINT32_C(0xedb88320));
    }
    return ~crc;
}

static void encode(ryz_display_settings_value_t v,uint8_t bytes[12])
{
    memcpy(bytes,"RYZD",4); bytes[4]=1; bytes[5]=v.rotation;
    bytes[6]=v.sleep_index; bytes[7]=v.brightness;
    uint32_t crc=checksum(bytes,8);
    for(unsigned i=0;i<4;++i) bytes[8+i]=(uint8_t)(crc>>(24-i*8));
}

static esp_err_t read_settings(ryz_display_settings_value_t *out)
{
    nvs_handle_t handle;
    esp_err_t error=nvs_open("ryz_display",NVS_READONLY,&handle);
    if(error==ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if(error!=ESP_OK) return error;
    uint8_t bytes[12]; size_t length=sizeof(bytes);
    error=nvs_get_blob(handle,"config",bytes,&length);
    nvs_close(handle);
    if(error==ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if(error==ESP_ERR_NVS_INVALID_LENGTH) return ESP_ERR_INVALID_ARG;
    if(error!=ESP_OK) return error;
    if(length!=sizeof(bytes) || memcmp(bytes,"RYZD",4) || bytes[4]!=1) return ESP_ERR_INVALID_ARG;
    uint32_t crc=0;
    for(unsigned i=0;i<4;++i) crc=(crc<<8)|bytes[8+i];
    if(crc!=checksum(bytes,8)) return ESP_ERR_INVALID_ARG;
    *out=(ryz_display_settings_value_t){.rotation=bytes[5],.sleep_index=bytes[6],.brightness=bytes[7]};
    return ryz_display_settings_valid(*out)?ESP_OK:ESP_ERR_INVALID_ARG;
}

esp_err_t ryz_display_settings_load_boot(ryz_display_settings_value_t *out)
{
    if(!out) return ESP_ERR_INVALID_ARG;
    *out=ryz_display_settings_defaults();
    esp_err_t error=nvs_flash_init();
    if(error!=ESP_OK) return error;
    ryz_display_settings_value_t stored={0};
    error=read_settings(&stored);
    if(error==ESP_OK) *out=stored;
    return error;
}

static esp_err_t write_settings(ryz_display_settings_value_t value,bool *confirmed)
{
    *confirmed=false;
    uint8_t bytes[12]; encode(value,bytes);
    nvs_handle_t handle;
    esp_err_t error=nvs_open("ryz_display",NVS_READWRITE,&handle);
    if(error!=ESP_OK) return error;
    error=nvs_set_blob(handle,"config",bytes,sizeof(bytes));
    if(error==ESP_OK) error=nvs_commit(handle);
    nvs_close(handle);
    /* Never equate set/accepted with SAVED. Read through a fresh handle and
     * verify the whole versioned CRC-protected tuple after successful commit. */
    ryz_display_settings_value_t observed={0};
    esp_err_t read_error=read_settings(&observed);
    if(error!=ESP_OK) return error;
    if(read_error!=ESP_OK) return read_error;
    if(!ryz_display_settings_equal(value,observed)) return ESP_ERR_INVALID_STATE;
    *confirmed=true; return ESP_OK;
}

static TaskHandle_t worker;
static bool core_ready;
static void worker_task(void *unused)
{
    (void)unused;
    for(;;) { ryz_display_settings_worker_tick(); vTaskDelay(pdMS_TO_TICKS(20)); }
}

esp_err_t ryz_display_settings_start(void)
{
    if(worker) return ESP_OK;
    if(!core_ready) {
        const ryz_display_settings_storage_t backend={.read=read_settings,.write=write_settings};
        esp_err_t error=ryz_display_settings_core_init(&backend);
        if(error!=ESP_OK) return error;
        core_ready=true;
    }
    return xTaskCreate(worker_task,"display_cfg",4096,NULL,2,&worker)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
