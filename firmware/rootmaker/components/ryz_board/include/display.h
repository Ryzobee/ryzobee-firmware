#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define RYZ_DISPLAY_WIDTH 240
#define RYZ_DISPLAY_HEIGHT 240

/* Single execution-owner interface. Canvas persists across Lua VMs, not resets.
 * Public colors are RGB565. show transfers the dirty window and waits for DMA
 * completion; unchanged frames do no IO. Reads inspect RAM, not LCD. */
esp_err_t ryz_display_init(void);
/* Bootstrap-only variant: program MADCTL/gap and brightness before the first
 * frame/backlight enable. The caller loads validated NVS settings; BSP does
 * not own persistence. An already-live display retains its configuration;
 * use apply_configuration for runtime changes. Same value ranges as below. */
esp_err_t ryz_display_init_with_configuration(uint8_t rotation, uint8_t brightness);
esp_err_t ryz_display_clear(uint16_t color);
/* Rectangle must fit inside the canvas; width/height must be positive. */
esp_err_t ryz_display_rect(int x, int y, int width, int height, uint16_t color);
/* Blit tightly packed native-endian RGB565 values. The driver converts them
 * to panel wire order. pixel_count must be exactly width * height. */
esp_err_t ryz_display_blit_rgb565_native(int x, int y, int width, int height,
                                        const uint16_t *pixels,
                                        size_t pixel_count);
/* Blit a tightly packed RGB565 image whose bytes are already in panel
 * big-endian order. The byte count must be exactly width * height * 2. */
esp_err_t ryz_display_blit_rgb565_be(int x, int y, int width, int height,
                                    const uint8_t *pixels,
                                    size_t byte_count);
/* Transparent 5x7 font, scale 1..6, uppercase ASCII/digits and basic punctuation.
 * Entire text must fit; invalid input is rejected before changing the canvas. */
esp_err_t ryz_display_text(int x, int y, const char *text, size_t length,
                           uint16_t color, int scale);
esp_err_t ryz_display_read_pixel(int x, int y, uint16_t *color);
esp_err_t ryz_display_show(void);
/* Trusted single display-owner PWM preview. Brightness 10..100, step 5.
 * Does not transfer pixels, alter orientation, or wake a sleeping display.
 * Success becomes the confirmed wake duty; persistence remains the caller's
 * responsibility. LEDC failures attempt to restore the old duty. An uncertain
 * rollback is reported by configuration_blocked(); an explicit brightness
 * retry can recover PWM, but cannot bypass a failed orientation transaction. */
esp_err_t ryz_display_set_brightness(uint8_t brightness);
/* Trusted SPI Owner only. Quarter turns clockwise, brightness10..100 step5.
 * Same-direction changes use PWM only. Rotation repaint is dark; failure
 * attempts the old complete configuration. If that
 * fails too, ordinary rendering/input must remain blocked until retry. */
esp_err_t ryz_display_apply_configuration(uint8_t rotation, uint8_t brightness);
bool ryz_display_configuration_blocked(void);
/* Backlight-only idle state. Neither turns off LCD logic nor stops Lua. */
esp_err_t ryz_display_set_asleep(bool asleep);
/* Boot-lifetime count of complete dirty-window transfers, including the real
 * initial frame. A transaction counts once only after its final DMA drain;
 * empty calls, failed/cancelled transfers and aborted stepped transfers do not
 * count. Repainting identical values still counts when it submits dirty data.
 * This relaxed atomic getter may be read by any task without touching hardware.
 * All other display operations retain their single SPI-owner contract. The
 * uint32_t value wraps naturally; consumers use unsigned deltas from their own
 * baseline, not the cumulative value as an FPS measurement. */
uint32_t ryz_display_completed_commits(void);
/* Trusted single SPI-owner privacy lifecycle; never expose these to Lua or
 * reenter them from a display cancellation callback.
 * Mark before the first sensitive canvas write. It performs no IO and rejects
 * an unready display, active stepped transfer or failed privacy barrier.
 * The caller must revoke sensitive input/text/LVGL references before clear. */
esp_err_t ryz_display_privacy_mark_sensitive(void);
/* No-op without a preceding mark or failed clear. Otherwise isolate ordinary
 * display access, request backlight duty 0 using LEDC, confirm pending DMA has
 * drained, wipe the whole canvas and BOTH DMA stripes, and submit a complete
 * black frame before restoring the confirmed duty (or zero while asleep). Only complete success
 * removes the isolation; errors remain explicitly retryable through this call
 * even after a prior DMA failure made the normal display unready. Ordinary
 * init/read/draw/show/step cannot bypass isolation or re-enable the backlight.
 * This does not rebuild the bus/panel or wipe caller/LVGL-owned memory.
 * SDK/LEDC failures do not prove that the physical screen is dark. The caller
 * must keep Lua/input handoff blocked on any error. A successfully transferred
 * black frame counts as a display commit even if later LEDC restoration fails. */
esp_err_t ryz_display_privacy_clear(void);
/* Cancellation never aborts an in-flight DMA; drain it before returning. */
esp_err_t ryz_display_show_checked(bool (*cancelled)(void *), void *context);
/* cancelled_out is true only when the callback requested cancellation and
 * every pending DMA transfer was drained successfully. A transport timeout
 * leaves it false even if cancellation was observed earlier. */
esp_err_t ryz_display_show_checked_status(
    bool (*cancelled)(void *), void *context, bool *cancelled_out);
/* Cooperative owner: transfer one stripe per call, preserving the old blocking
 * interface. No drawing until done or abort. All calls stay on the SPI owner core. */
esp_err_t ryz_display_show_step(bool begin, bool *done);
void ryz_display_show_abort(void);
