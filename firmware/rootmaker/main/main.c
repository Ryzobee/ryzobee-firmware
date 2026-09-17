#include "display.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "ryz_font.h"
#include "ryz_display_settings.h"
#include "ryz_json_memory.h"
#include "touch.h"
#include "workbench.h"

#if CONFIG_RYZ_BOOT_DIAGNOSTICS
extern const char system_boot_diag_start[] asm("_binary_system_boot_diag_lua_start");
extern const char display_demo_start[] asm("_binary_display_demo_lua_start");
extern const char touch_demo_start[] asm("_binary_touch_demo_lua_start");
#define RYZ_DISPLAY_BOOT_SOURCE display_demo_start
#define RYZ_TOUCH_BOOT_SOURCE touch_demo_start
#define RYZ_SYSTEM_BOOT_SOURCE system_boot_diag_start
#else
#define RYZ_DISPLAY_BOOT_SOURCE NULL
#define RYZ_TOUCH_BOOT_SOURCE NULL
#define RYZ_SYSTEM_BOOT_SOURCE NULL
#endif

void app_main(void)
{
    /* Read only the saved display tuple before panel initialization, not after
     * wireless restoration. Missing/corrupt NVS leaves validated defaults;
     * never erase NVS to make the splash work. No application task exists yet. */
    ryz_display_settings_value_t boot_display;
    esp_err_t boot_display_status = ryz_display_settings_load_boot(&boot_display);
    esp_err_t display_status = ryz_display_init_with_configuration(
        boot_display.rotation, boot_display.brightness);
    /* Set the JSON-only allocator once, before UI/HTTP/radio tasks exist.
     * Keep the panel first and keep ordinary malloc/DMA allocation unchanged. */
    ryz_json_memory_init();
    bool display_ready = display_status == ESP_OK;
#if !CONFIG_RYZ_BOOT_DIAGNOSTICS
    if (display_ready) {
        esp_err_t splash_status = ryz_workbench_start_display();
        ESP_LOGI("ryzobee", "boot splash first frame: %s", esp_err_to_name(splash_status));
    }
#endif
    ESP_LOGI("ryzobee", "display driver: %s (visible output requires observation)",
             esp_err_to_name(display_status));
    if (boot_display_status != ESP_OK && boot_display_status != ESP_ERR_NOT_FOUND)
        ESP_LOGW("ryzobee", "boot display settings: %s; using defaults",
                 esp_err_to_name(boot_display_status));

    const esp_vfs_spiffs_conf_t filesystem = {
        .base_path = "/scripts",
        .partition_label = "scripts",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    bool filesystem_ready = esp_vfs_spiffs_register(&filesystem) == ESP_OK;
    ryz_workbench_prepare();

    /* The CST816 reports physical coordinates. Map them to the panel's logical
     * orientation before its initialization performs the first touch read. */
    esp_err_t touch_status = ryz_touch_set_rotation(boot_display.rotation);
    if (touch_status == ESP_OK) touch_status = ryz_touch_init();
    bool touch_ready = touch_status == ESP_OK;
    ESP_LOGI("ryzobee", "touch driver: %s", esp_err_to_name(touch_status));

    esp_err_t font_status = ESP_OK; /* Static fonts have no runtime init. */
#if CONFIG_RYZ_LUA_FREETYPE
    font_status = ryz_font_init();
    ESP_LOGI("ryzobee", "font engine: %s", esp_err_to_name(font_status));
#else
    ESP_LOGI("ryzobee", "static UI fonts; FreeType disabled");
#endif

    const ryz_workbench_config_t workbench = {
        .filesystem_ready = filesystem_ready,
        .display_ready = display_ready,
        .touch_ready = touch_ready,
        .font_ready = font_status == ESP_OK,
        .display_boot_source = RYZ_DISPLAY_BOOT_SOURCE,
        .touch_boot_source = RYZ_TOUCH_BOOT_SOURCE,
        .system_boot_source = RYZ_SYSTEM_BOOT_SOURCE,
    };
    ryz_workbench_start(&workbench);
}
