#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Native RGB565 RAM canvas and separately presented pixels. Only the physical
 * transfer is simulated; all LVGL layout/rasterization uses the locked library. */
void ryz_host_display_reset(void);
void ryz_host_display_advance_ms(uint32_t milliseconds);
uint16_t ryz_host_display_pixel(unsigned x, unsigned y);
const uint16_t *ryz_host_display_frame(void);
size_t ryz_host_display_blits(void);
size_t ryz_host_display_blit_pixels(void);
size_t ryz_host_display_shows(void);
size_t ryz_host_display_rows(void);
void ryz_host_display_fail_blit(esp_err_t error);
void ryz_host_display_fail_show(esp_err_t error, unsigned transferred_rows);
void ryz_host_display_fail_cancel_drain(esp_err_t error);
/* Coarse SDK/BSP endpoint for UI failure tests. Actual LEDC, DMA and scrub
 * ordering are independently tested against production display.c. */
void ryz_host_display_fail_privacy(esp_err_t error);
bool ryz_host_display_privacy_blocked(void);
/* Diagnostic export of the actually presented frame; never feeds rendering. */
void ryz_host_display_save(const char *directory, const char *name);
