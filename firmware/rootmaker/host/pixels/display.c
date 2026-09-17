#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_timer.h"
#include "ryz_display_host.h"

static uint16_t canvas[RYZ_DISPLAY_WIDTH * RYZ_DISPLAY_HEIGHT];
static uint16_t presented[RYZ_DISPLAY_WIDTH * RYZ_DISPLAY_HEIGHT];
static int64_t now_us;
static size_t blits, shows, rows, blit_pixels;
static esp_err_t blit_error, show_error, cancel_drain_error;
static unsigned error_after_rows;
static bool privacy_tainted, privacy_blocked;
static esp_err_t privacy_error;

void ryz_host_display_reset(void)
{
    memset(canvas, 0, sizeof(canvas));
    memset(presented, 0, sizeof(presented));
    blits = shows = rows = blit_pixels = 0;
    blit_error = show_error = cancel_drain_error = ESP_OK;
    error_after_rows = 0;
    privacy_tainted=privacy_blocked=false;
    privacy_error=ESP_OK;
}

void ryz_host_display_fail_privacy(esp_err_t error) { privacy_error=error; }
bool ryz_host_display_privacy_blocked(void) { return privacy_blocked; }
esp_err_t ryz_display_privacy_mark_sensitive(void)
{
    if(privacy_blocked) return ESP_ERR_INVALID_STATE;
    privacy_tainted=true;
    return ESP_OK;
}
esp_err_t ryz_display_privacy_clear(void)
{
    if(!privacy_tainted && !privacy_blocked) return ESP_OK;
    privacy_blocked=true;
    if(privacy_error!=ESP_OK) return privacy_error;
    memset(canvas,0,sizeof(canvas));
    memset(presented,0,sizeof(presented));
    ++shows;
    rows+=RYZ_DISPLAY_HEIGHT;
    privacy_tainted=privacy_blocked=false;
    return ESP_OK;
}
esp_err_t ryz_display_read_pixel(int x,int y,uint16_t *color)
{
    if(privacy_blocked) return ESP_ERR_INVALID_STATE;
    if(!color || x<0 || y<0 || x>=RYZ_DISPLAY_WIDTH || y>=RYZ_DISPLAY_HEIGHT)
        return ESP_ERR_INVALID_ARG;
    *color=canvas[y*RYZ_DISPLAY_WIDTH+x];
    return ESP_OK;
}

int64_t esp_timer_get_time(void) { return now_us; }
void ryz_host_display_advance_ms(uint32_t milliseconds)
{
    now_us += (int64_t)milliseconds * 1000;
}

uint16_t ryz_host_display_pixel(unsigned x, unsigned y)
{
    assert(x < RYZ_DISPLAY_WIDTH && y < RYZ_DISPLAY_HEIGHT);
    return presented[y * RYZ_DISPLAY_WIDTH + x];
}
const uint16_t *ryz_host_display_frame(void) { return presented; }
size_t ryz_host_display_blits(void) { return blits; }
size_t ryz_host_display_blit_pixels(void) { return blit_pixels; }
size_t ryz_host_display_shows(void) { return shows; }
size_t ryz_host_display_rows(void) { return rows; }
void ryz_host_display_fail_blit(esp_err_t error) { blit_error = error; }
void ryz_host_display_fail_show(esp_err_t error, unsigned transferred_rows)
{
    assert(transferred_rows <= RYZ_DISPLAY_HEIGHT);
    show_error = error;
    error_after_rows = transferred_rows;
}
void ryz_host_display_fail_cancel_drain(esp_err_t error)
{
    cancel_drain_error = error;
}

esp_err_t ryz_display_blit_rgb565_native(int x, int y, int width, int height,
                                        const uint16_t *pixels, size_t pixel_count)
{
    if(privacy_blocked) return ESP_ERR_INVALID_STATE;
    ++blits;
    if (!pixels || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x > RYZ_DISPLAY_WIDTH - width || y > RYZ_DISPLAY_HEIGHT - height ||
        pixel_count != (size_t)width * (size_t)height) return ESP_ERR_INVALID_ARG;
    blit_pixels += pixel_count;
    if (blit_error != ESP_OK) {
        const esp_err_t error = blit_error;
        blit_error = ESP_OK;
        return error;
    }
    for (int row = 0; row < height; ++row) {
        memcpy(canvas + (y + row) * RYZ_DISPLAY_WIDTH + x,
               pixels + row * width, (size_t)width * sizeof(uint16_t));
    }
    return ESP_OK;
}

esp_err_t ryz_display_show_checked_status(bool (*cancelled)(void *),
                                          void *context, bool *cancelled_out)
{
    if (cancelled_out) *cancelled_out = false;
    if(privacy_blocked) return ESP_ERR_INVALID_STATE;
    ++shows;
    const esp_err_t pending_error = show_error;
    const unsigned fail_at = error_after_rows;
    show_error = ESP_OK;
    for (unsigned y = 0; y < RYZ_DISPLAY_HEIGHT; y += 16U) {
        if (pending_error != ESP_OK && y >= fail_at) return pending_error;
        if (cancelled && cancelled(context)) {
            if (cancel_drain_error != ESP_OK) {
                const esp_err_t error = cancel_drain_error;
                cancel_drain_error = ESP_OK;
                return error; /* The production BSP gives drain errors priority. */
            }
            if (cancelled_out) *cancelled_out = true;
            return ESP_ERR_TIMEOUT;
        }
        memcpy(presented + y * RYZ_DISPLAY_WIDTH,
               canvas + y * RYZ_DISPLAY_WIDTH,
               16U * RYZ_DISPLAY_WIDTH * sizeof(uint16_t));
        rows += 16U;
    }
    return pending_error;
}

esp_err_t ryz_display_show_checked(bool (*cancelled)(void *), void *context)
{
    return ryz_display_show_checked_status(cancelled, context, NULL);
}

static void put_le(FILE *file, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) assert(fputc((value >> (i * 8)) & 0xff, file) != EOF);
}

void ryz_host_display_save(const char *directory, const char *name)
{
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s.bmp", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file && fwrite("BM", 1, 2, file) == 2);
    put_le(file, 54 + 240 * 240 * 3, 4); put_le(file, 0, 4); put_le(file, 54, 4);
    put_le(file, 40, 4); put_le(file, 240, 4); put_le(file, 240, 4);
    put_le(file, 1, 2); put_le(file, 24, 2);
    for (unsigned i = 0; i < 6; ++i) put_le(file, 0, 4);
    for (int y = 239; y >= 0; --y) {
        for (int x = 0; x < 240; ++x) {
            uint16_t color = presented[y * 240 + x];
            uint8_t b = color & 31, g = (color >> 5) & 63, r = color >> 11;
            assert(fputc((b << 3) | (b >> 2), file) != EOF);
            assert(fputc((g << 2) | (g >> 4), file) != EOF);
            assert(fputc((r << 3) | (r >> 2), file) != EOF);
        }
    }
    assert(fclose(file) == 0);
    printf("PIXEL_ARTIFACT %s\n", path);
    length = snprintf(path, sizeof(path), "%s/%s.rgb565", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    file = fopen(path, "wb");
    assert(file);
    for (size_t i = 0; i < 240 * 240; ++i) put_le(file, presented[i], 2);
    assert(fclose(file) == 0);
}
