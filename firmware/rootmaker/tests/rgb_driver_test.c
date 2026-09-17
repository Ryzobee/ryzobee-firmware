#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ryz_rgb_driver.h"
#include "driver/rmt_tx.h"
#include "freertos/task.h"

static int channel_token, encoder_token;
static unsigned new_channels, new_encoders, enables, disables, transmissions, waits;
static unsigned deleted_channels, deleted_encoders;
static esp_err_t delete_channel_error, delete_encoder_error;
static bool channel_live, encoder_live;
static esp_err_t channel_error, encoder_error, enable_error, transmit_error, resolution_error;
static unsigned fail_disables;
static bool fail_pm_release, stuck_wait;
static esp_err_t wait_errors[8];
static unsigned wait_error_count, wait_error_index;
static uint32_t resolution = 80000000;
static bool enabled, pending;
static const rmt_symbol_word_t *retained;
static rmt_symbol_word_t saved[26];
static ryz_rgb_color_t expected_color;
static int owner_token;
static TaskHandle_t current_task = &owner_token;
static BaseType_t current_core = 1, affinity = 1;
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
BaseType_t xTaskGetCoreID(TaskHandle_t task) { assert(!task); return affinity; }
BaseType_t xPortGetCoreID(void) { return current_core; }

static void immutable(void)
{
    if (pending) assert(retained && !memcmp(retained, saved, sizeof(saved)));
}

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *out)
{
    ++new_channels;
    assert(config->gpio_num == 45 && config->clk_src == RMT_CLK_SRC_APB);
    assert(config->resolution_hz == 80000000 && config->mem_block_symbols == 48);
    assert(config->trans_queue_depth == 1 && !*out);
    if (channel_error) return channel_error;
    assert(!channel_live);
    channel_live = true;
    *out = &channel_token;
    return ESP_OK;
}
esp_err_t rmt_get_channel_resolution(rmt_channel_handle_t channel, uint32_t *out)
{
    assert(channel == &channel_token);
    immutable();
    *out = resolution;
    return resolution_error;
}
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *config, rmt_encoder_handle_t *out)
{
    assert(config && !*out);
    ++new_encoders;
    if (encoder_error) return encoder_error;
    assert(!encoder_live);
    encoder_live = true;
    *out = &encoder_token;
    return ESP_OK;
}
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder)
{
    assert(encoder == &encoder_token && encoder_live && !enabled && !pending);
    ++deleted_encoders;
    if (delete_encoder_error) return delete_encoder_error;
    encoder_live = false;
    return ESP_OK;
}
esp_err_t rmt_del_channel(rmt_channel_handle_t channel)
{
    assert(channel == &channel_token && channel_live && !enabled && !pending);
    ++deleted_channels;
    if (delete_channel_error) return delete_channel_error;
    channel_live = false;
    return ESP_OK;
}
esp_err_t rmt_enable(rmt_channel_handle_t channel)
{
    assert(channel == &channel_token && !enabled && !pending);
    ++enables;
    if (enable_error) return enable_error;
    enabled = true;
    return ESP_OK;
}
esp_err_t rmt_disable(rmt_channel_handle_t channel)
{
    assert(channel == &channel_token && enabled);
    ++disables;
    immutable();
    /* IDF's PM-release failure occurs after stop/recycling, but before the
     * public channel FSM returns to INIT. It may remain WAIT indefinitely. */
    if (stuck_wait) return ESP_ERR_INVALID_STATE;
    if (fail_pm_release) {
        stuck_wait = true;
        return ESP_FAIL;
    }
    if (fail_disables) {
        --fail_disables;
        return ESP_ERR_INVALID_STATE;
    }
    enabled = false;
    return ESP_OK;
}
esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder,
                       const void *payload, size_t size, const rmt_transmit_config_t *config)
{
    assert(channel == &channel_token && encoder == &encoder_token && enabled && !pending);
    assert(size == sizeof(saved) && config->flags.queue_nonblocking && !config->flags.eot_level);
    assert(!config->loop_count);
    ++transmissions;
    const rmt_symbol_word_t *words = payload;
    for (unsigned i = 0; i < 26; i += 25) {
        assert(words[i].level0 == 0 && words[i].level1 == 0);
        assert(words[i].duration0 == 14000 && words[i].duration1 == 14000);
    }
    const uint8_t bytes[] = {expected_color.green, expected_color.red, expected_color.blue};
    for (unsigned byte = 0; byte < 3; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const bool one = (bytes[byte] & (0x80U >> bit)) != 0;
            const rmt_symbol_word_t w = words[1 + 8 * byte + bit];
            assert(w.level0 == 1 && w.level1 == 0);
            assert(w.duration0 == (one ? 56 : 28) && w.duration1 == (one ? 47 : 72));
        }
    }
    if (transmit_error) return transmit_error;
    pending = true;
    retained = payload;
    memcpy(saved, payload, sizeof(saved));
    return ESP_OK;
}
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout)
{
    assert(channel == &channel_token && timeout == 20);
    ++waits;
    immutable();
    if (wait_error_index < wait_error_count) {
        esp_err_t error = wait_errors[wait_error_index++];
        if (error != ESP_OK) return error;
    }
    pending = false;
    retained = NULL;
    return ESP_OK;
}

static esp_err_t write_color(uint8_t r, uint8_t g, uint8_t b)
{
    expected_color = (ryz_rgb_color_t){r, g, b};
    return ryz_rgb_driver_write(expected_color);
}
static void set_wait_errors(esp_err_t first, esp_err_t second, esp_err_t third)
{
    wait_errors[0] = first;
    wait_errors[1] = second;
    wait_errors[2] = third;
    wait_error_count = 3;
    wait_error_index = 0;
}
static void waveform(void)
{
    for (unsigned v = 0; v < 256; ++v) {
        assert(write_color(v, 0, 0) == ESP_OK);
        assert(write_color(0, v, 0) == ESP_OK);
        assert(write_color(0, 0, v) == ESP_OK);
        assert(write_color(v, 255 - v, v ^ 0x55) == ESP_OK);
    }
    assert(new_channels == 1 && new_encoders == 1 && transmissions == 1024);
    assert(enables == 1024 && disables == 1024 && !enabled && !pending);
}
static void partial_channel(void)
{
    channel_error = ESP_ERR_NO_MEM;
    assert(write_color(1,2,3) == ESP_ERR_NO_MEM && !new_encoders && !transmissions);
    channel_error = ESP_OK;
    assert(write_color(1,2,3) == ESP_OK && new_channels == 2 && new_encoders == 1);
}
static void partial_encoder(void)
{
    encoder_error = ESP_ERR_NO_MEM;
    assert(write_color(1,2,3) == ESP_ERR_NO_MEM && !enables && !transmissions);
    encoder_error = ESP_OK;
    assert(write_color(1,2,3) == ESP_OK && new_channels == 1 && new_encoders == 2);
}
static void wrong_clock(void)
{
    resolution = 40000000;
    assert(write_color(1,2,3) == ESP_ERR_NOT_SUPPORTED);
    assert(write_color(4,5,6) == ESP_ERR_NOT_SUPPORTED);
    assert(new_channels == 1 && !new_encoders && !enables && !transmissions);
    resolution_error = ESP_FAIL;
    assert(write_color(1,2,3) == ESP_FAIL && !transmissions);
    resolution_error = ESP_OK;
    resolution = 80000000;
    assert(write_color(1,2,3) == ESP_OK && new_channels == 1);
}
static void admission(void)
{
    enable_error = ESP_FAIL;
    assert(write_color(1,2,3) == ESP_FAIL && !transmissions && !disables);
    enable_error = ESP_OK;
    transmit_error = ESP_ERR_INVALID_STATE;
    assert(write_color(1,2,3) == ESP_ERR_INVALID_STATE && !enabled && !pending);
    assert(waits == 1 && disables == 1);
    transmit_error = ESP_OK;
    assert(write_color(4,5,6) == ESP_OK && transmissions == 2);
}
static void timeout_stopped(void)
{
    set_wait_errors(ESP_ERR_TIMEOUT, ESP_OK, ESP_OK);
    assert(write_color(255,0,0) == ESP_ERR_TIMEOUT);
    assert(!enabled && !pending && waits == 2 && disables == 1);
    assert(write_color(0,0,0) == ESP_OK && transmissions == 2);
}
static void timeout_disable_failure(void)
{
    set_wait_errors(ESP_ERR_TIMEOUT, ESP_OK, ESP_OK);
    fail_disables = 2;
    assert(write_color(255,0,0) == ESP_ERR_TIMEOUT && pending && enabled);
    const unsigned old_waits = waits;
    assert(write_color(0,255,0) == ESP_ERR_INVALID_STATE);
    assert(pending && enabled && transmissions == 1 && waits == old_waits);
    immutable();
    assert(write_color(0,0,0) == ESP_OK && transmissions == 2 && !pending && !enabled);
}
static void timeout_reclaim_failure(void)
{
    set_wait_errors(ESP_ERR_TIMEOUT, ESP_ERR_TIMEOUT, ESP_ERR_TIMEOUT);
    assert(write_color(255,0,0) == ESP_ERR_TIMEOUT && pending && !enabled);
    assert(write_color(0,255,0) == ESP_ERR_TIMEOUT && transmissions == 1);
    assert(enables == 1 && disables == 1 && pending);
    immutable();
    assert(write_color(0,0,0) == ESP_OK && transmissions == 2 && !pending && !enabled);
}
static void completed_disable_failure(void)
{
    fail_disables = 2;
    assert(write_color(255,0,0) == ESP_ERR_INVALID_STATE && !pending && enabled);
    assert(write_color(0,255,0) == ESP_ERR_INVALID_STATE && transmissions == 1);
    assert(write_color(0,0,0) == ESP_OK && transmissions == 2 && !pending && !enabled);
}
static void permanent_disable_failure(void)
{
    fail_pm_release = true;
    assert(write_color(255,0,0) == ESP_FAIL && !pending && enabled && stuck_wait);
    for (unsigned i = 0; i < 8; ++i) {
        assert(write_color(0,0,0) == ESP_ERR_INVALID_STATE);
        assert(transmissions == 1 && enables == 1 && waits == 1);
    }
    /* No claim that a stuck disabled FSM can automatically be recovered, or
     * that a requested black frame reached the LED. Handles remain retained. */
}
static void cleanup_complete(void)
{
    assert(write_color(25, 90, 210) == ESP_OK);
    assert(ryz_rgb_driver_get_status().resources_held);
    assert(ryz_rgb_driver_cleanup() == ESP_OK);
    assert(!ryz_rgb_driver_get_status().resources_held);
    assert(deleted_channels == 1 && deleted_encoders == 1 && transmissions == 1);
    assert(ryz_rgb_driver_cleanup() == ESP_OK);
    assert(deleted_channels == 1 && deleted_encoders == 1 && transmissions == 1);
    assert(write_color(45, 65, 85) == ESP_OK);
    assert(new_channels == 2 && new_encoders == 2 && transmissions == 2);
}
static void cleanup_empty_partial(void)
{
    assert(ryz_rgb_driver_cleanup() == ESP_OK);
    assert(!new_channels && !new_encoders && !deleted_channels && !deleted_encoders && !transmissions);
    encoder_error = ESP_ERR_NO_MEM;
    assert(write_color(15, 35, 75) == ESP_ERR_NO_MEM);
    assert(ryz_rgb_driver_get_status().resources_held);
    assert(ryz_rgb_driver_cleanup() == ESP_OK);
    assert(!ryz_rgb_driver_get_status().resources_held && deleted_channels == 1 && !deleted_encoders);
    assert(!transmissions && !enables);
}
static void cleanup_timeout(void)
{
    set_wait_errors(ESP_ERR_TIMEOUT, ESP_OK, ESP_OK);
    fail_disables = 1;
    assert(write_color(15, 35, 75) == ESP_ERR_TIMEOUT && enabled && pending);
    ryz_rgb_driver_status_t status = ryz_rgb_driver_get_status();
    assert(status.resources_held && status.cleanup_error == ESP_ERR_INVALID_STATE);
    assert(ryz_rgb_driver_cleanup() == ESP_OK);
    status = ryz_rgb_driver_get_status();
    assert(!status.resources_held && status.cleanup_error == ESP_OK);
    assert(transmissions == 1 && deleted_channels == 1 && deleted_encoders == 1);
}
static void cleanup_drain_retry(void)
{
    set_wait_errors(ESP_ERR_TIMEOUT, ESP_ERR_TIMEOUT, ESP_ERR_TIMEOUT);
    assert(write_color(15, 35, 75) == ESP_ERR_TIMEOUT && !enabled && pending);
    assert(ryz_rgb_driver_cleanup() == ESP_ERR_TIMEOUT);
    immutable();
    ryz_rgb_driver_status_t status = ryz_rgb_driver_get_status();
    assert(status.resources_held && status.cleanup_error == ESP_ERR_TIMEOUT);
    assert(transmissions == 1 && !deleted_channels && !deleted_encoders);
    assert(ryz_rgb_driver_cleanup() == ESP_OK);
    assert(!ryz_rgb_driver_get_status().resources_held);
    assert(transmissions == 1 && deleted_channels == 1 && deleted_encoders == 1);
}
static void cleanup_delete_retry(bool encoder)
{
    assert(write_color(15, 35, 75) == ESP_OK);
    if (encoder) delete_encoder_error = ESP_FAIL;
    else delete_channel_error = ESP_FAIL;
    assert(ryz_rgb_driver_cleanup() == ESP_FAIL);
    ryz_rgb_driver_status_t status = ryz_rgb_driver_get_status();
    assert(status.resources_held && status.cleanup_error == ESP_FAIL);
    assert(deleted_encoders == 1 && deleted_channels == (encoder ? 0U : 1U));
    /* A failed channel destroy may already disconnect GPIO. A later write
     * must retry teardown rather than reenable/reuse that partial channel. */
    assert(write_color(25, 45, 85) == ESP_FAIL);
    assert(transmissions == 1 && enables == 1 && new_channels == 1 && new_encoders == 1);
    delete_encoder_error = delete_channel_error = ESP_OK;
    assert(ryz_rgb_driver_cleanup() == ESP_OK);
    assert(!ryz_rgb_driver_get_status().resources_held);
    assert(deleted_encoders == (encoder ? 3U : 1U) && deleted_channels == (encoder ? 1U : 3U));
    assert(write_color(25, 45, 85) == ESP_OK && new_channels == 2 && new_encoders == 2);
}
static void cleanup_permanent_stop_failure(void)
{
    fail_pm_release = true;
    assert(write_color(15, 35, 75) == ESP_FAIL && enabled);
    for (unsigned i = 0; i < 8; ++i) {
        assert(ryz_rgb_driver_cleanup() == ESP_ERR_INVALID_STATE);
        ryz_rgb_driver_status_t status = ryz_rgb_driver_get_status();
        assert(status.resources_held && status.cleanup_error == ESP_ERR_INVALID_STATE);
        assert(!deleted_channels && !deleted_encoders && transmissions == 1);
    }
}
static void owner_guard(void)
{
    current_core = 0;
    assert(write_color(15, 35, 75) == ESP_ERR_INVALID_STATE);
    assert(ryz_rgb_driver_cleanup() == ESP_ERR_INVALID_STATE && !new_channels);
    current_core = 1;
    affinity = -1;
    assert(write_color(15, 35, 75) == ESP_ERR_INVALID_STATE && !new_channels);
    affinity = 1;
    assert(write_color(15, 35, 75) == ESP_OK);
    int other;
    current_task = &other;
    assert(ryz_rgb_driver_cleanup() == ESP_ERR_INVALID_STATE);
    assert(write_color(25, 45, 85) == ESP_ERR_INVALID_STATE);
    assert(transmissions == 1 && !deleted_channels && !deleted_encoders);
    current_task = &owner_token;
    assert(ryz_rgb_driver_cleanup() == ESP_OK && !ryz_rgb_driver_get_status().resources_held);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "waveform")) waveform();
    else if (!strcmp(argv[1], "partial_channel")) partial_channel();
    else if (!strcmp(argv[1], "partial_encoder")) partial_encoder();
    else if (!strcmp(argv[1], "wrong_clock")) wrong_clock();
    else if (!strcmp(argv[1], "admission")) admission();
    else if (!strcmp(argv[1], "timeout_stopped")) timeout_stopped();
    else if (!strcmp(argv[1], "timeout_disable_failure")) timeout_disable_failure();
    else if (!strcmp(argv[1], "timeout_reclaim_failure")) timeout_reclaim_failure();
    else if (!strcmp(argv[1], "completed_disable_failure")) completed_disable_failure();
    else if (!strcmp(argv[1], "permanent_disable_failure")) permanent_disable_failure();
    else if (!strcmp(argv[1], "cleanup_complete")) cleanup_complete();
    else if (!strcmp(argv[1], "cleanup_empty_partial")) cleanup_empty_partial();
    else if (!strcmp(argv[1], "cleanup_timeout")) cleanup_timeout();
    else if (!strcmp(argv[1], "cleanup_drain_retry")) cleanup_drain_retry();
    else if (!strcmp(argv[1], "cleanup_encoder_retry")) cleanup_delete_retry(true);
    else if (!strcmp(argv[1], "cleanup_channel_retry")) cleanup_delete_retry(false);
    else if (!strcmp(argv[1], "cleanup_permanent_stop_failure")) cleanup_permanent_stop_failure();
    else if (!strcmp(argv[1], "owner_guard")) owner_guard();
    else assert(false);
    printf("RGB_DRIVER_PASS %s\n", argv[1]);
    return 0;
}
