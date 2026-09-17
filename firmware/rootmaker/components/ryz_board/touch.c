/* The electrical setup matches ggadc/Ryzobee_arduino_esp32 8ec48fd: I2C0 on
 * GPIO41/40, INT1, active-low RST2 with 10 ms low + 50 ms startup. That library
 * stores a 400 kHz value in its LovyanGFX config but its custom CST816 driver
 * uses an external Wire instance without applying the value. Keep the proven
 * 100 kHz bus here, plus the local D/T identity and DisAutoSleep checks. */
#include "touch.h"
#include "board_i2c.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOUCH_RESET GPIO_NUM_2
#define TOUCH_INTERRUPT GPIO_NUM_1
static ryz_touch_info_t status;
static bool previous_pressed;
static bool previous_valid;
static uint8_t touch_rotation;

esp_err_t ryz_touch_set_rotation(uint8_t rotation)
{
    if (rotation > 3) return ESP_ERR_INVALID_ARG;
    touch_rotation = rotation;
    previous_valid = false;
    return ESP_OK;
}

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t length)
{
    return ryz_board_i2c_read_reg(RYZ_BOARD_I2C_TOUCH, reg, data, length);
}

esp_err_t ryz_touch_init(void)
{
    if (status.ready) return ESP_OK;
    memset(&status, 0, sizeof(status));
    previous_valid = previous_pressed = false;
    esp_err_t err = ryz_board_i2c_init();
    if (err != ESP_OK) return err;
    const gpio_config_t reset = {
        .pin_bit_mask = 1ULL << TOUCH_RESET, .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t interrupt = {
        .pin_bit_mask = 1ULL << TOUCH_INTERRUPT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    err = gpio_config(&reset);
    if (err != ESP_OK) goto failed;
    err = gpio_config(&interrupt);
    if (err != ESP_OK) goto failed;
    err = gpio_set_level(TOUCH_RESET, 0);
    if (err != ESP_OK) goto failed;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(TOUCH_RESET, 1);
    if (err != ESP_OK) goto failed;
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t identity[4];
    err = read_register(0xa7, identity, sizeof(identity));
    if (err != ESP_OK) goto failed;
    status.chip_id = identity[0];
    status.project_id = identity[1];
    status.firmware_version = identity[2];
    status.factory_id = identity[3];
    ESP_LOGI("touch", "CST816 identity: chip=0x%02x project=0x%02x fw=0x%02x factory=0x%02x",
             identity[0], identity[1], identity[2], identity[3]);
    if (!ryz_touch_controller_name(status.chip_id)) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto failed;
    }
    /* CST816T SDK v1.3 and Elecrow CST816D sample: DisAutoSleep=1.
     * This volatile setting trades low-power idle for reliable polling. */
    const uint8_t awake[] = {0xfe, 0x01};
    err = ryz_board_i2c_write(RYZ_BOARD_I2C_TOUCH, awake, sizeof(awake));
    if (err != ESP_OK) goto failed;
    err = read_register(0xfe, &status.disable_auto_sleep, 1);
    if (err != ESP_OK) goto failed;
    if (status.disable_auto_sleep != 1) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto failed;
    }
    status.ready = true;
    ryz_touch_sample_t initial;
    err = ryz_touch_read(&initial);
    if (err == ESP_OK) return ESP_OK;
failed:
    status.ready = false;
    /* The bus/device are board-owned and may already serve the IMU. A touch
     * fault must not remove their driver or reconfigure the shared pins. */
    return err;
}

esp_err_t ryz_touch_read(ryz_touch_sample_t *sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    memset(sample, 0, sizeof(*sample));
    if (!status.ready) return ESP_ERR_INVALID_STATE;
    uint8_t data[6];
    esp_err_t err = read_register(0x01, data, sizeof(data));
    if (err == ESP_ERR_TIMEOUT) {
        /* The touch controller shares I2C0 with the IMU. A single transaction
         * can time out while that shared bus recovers; do one bounded retry so
         * a transient transport fault does not tear down a running app. Never
         * retry malformed data or a persistent fault, and never reuse bytes
         * from the failed attempt. */
        TickType_t ticks = pdMS_TO_TICKS(1);
        vTaskDelay(ticks ? ticks : 1);
        err = read_register(0x01, data, sizeof(data));
    }
    if (err == ESP_OK && (!ryz_touch_decode(data, sample) ||
                         !ryz_touch_rotate(sample, touch_rotation))) err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) {
        ++status.errors;
        previous_valid = false;
        memset(sample, 0, sizeof(*sample));
        return err; /* Never pass an old pressed coordinate off as a fresh read. */
    }
    sample->sampled_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ++status.reads;
    ryz_touch_normalize_event(previous_valid, previous_pressed, sample);
    if (sample->event == RYZ_TOUCH_DOWN) ++status.presses;
    if (sample->event == RYZ_TOUCH_UP) ++status.releases;
    previous_pressed = sample->pressed;
    previous_valid = true;
    return ESP_OK;
}

void ryz_touch_get_info(ryz_touch_info_t *info)
{
    if (info) *info = status;
}
