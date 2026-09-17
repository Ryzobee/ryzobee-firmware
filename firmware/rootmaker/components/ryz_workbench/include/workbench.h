#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool filesystem_ready;
    bool display_ready;
    bool touch_ready;
    bool font_ready;
    const char *display_boot_source;
    const char *touch_boot_source;
    /* Optional immutable app-image diagnostic; never a user filesystem path. */
    const char *system_boot_source;
} ryz_workbench_config_t;

/* After successful LCD init, start the eventual UI Owner on this SPI core.
 * Returns after its first splash frame (or render error). Does not start the
 * transport/services or access filesystem, touch, fonts or workbench queues.
 * Call once from app_main; diagnostics builds keep the legacy boot demos. */
esp_err_t ryz_workbench_start_display(void);

/* Installs the sole UART receiver before protocol diagnostics. */
void ryz_workbench_prepare(void);

/* Owns boot protocol, script/file transport, Console and UART until reset. */
void ryz_workbench_start(const ryz_workbench_config_t *config);
