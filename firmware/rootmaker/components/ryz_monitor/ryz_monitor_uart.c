#include "ryz_monitor_source.h"
#include "ryz_tool_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/uart_ll.h" /* IRQ mask enum only; no register access here. */
#include "sdkconfig.h"
#include <string.h>

#if !CONFIG_IDF_TARGET_ESP32S3 || ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "Monitor UART requires the audited ESP32-S3 / ESP-IDF 5.5.4"
#endif
#if CONFIG_PM_ENABLE || CONFIG_FREERTOS_SMP || CONFIG_FREERTOS_UNICORE
#error "Monitor UART requires the audited dual-core, non-SMP, non-PM configuration"
#endif
#if CONFIG_ESP_CONSOLE_UART_NUM == 1 || CONFIG_APPTRACE_DEST_UART1
#error "Monitor UART1 cannot own a console or application trace UART"
#endif

static QueueHandle_t s_queue;
static TaskHandle_t s_owner;
static uint32_t s_lease, s_controller_lease;
static int s_rx;
static bool s_installed, s_restore_rx, s_active;

static bool owner_valid(void)
{
    return xTaskGetCoreID(NULL) == 1 && xPortGetCoreID() == 1 &&
        (!s_owner || s_owner == xTaskGetCurrentTaskHandle());
}

esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (config->source == RYZ_MONITOR_SYSTEM) {
        return config->rx == -1 && config->tx == -1 && config->baud == 0 ?
            ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (config->source != RYZ_MONITOR_UART || config->rx == config->tx ||
        ryz_tool_pin_reason(config->rx) != RYZ_TOOL_PIN_AVAILABLE ||
        ryz_tool_pin_reason(config->tx) != RYZ_TOOL_PIN_AVAILABLE) return ESP_ERR_INVALID_ARG;
    switch (config->baud) {
    case 1200: case 2400: case 4800: case 9600: case 19200: case 38400:
    case 57600: case 115200: case 230400: case 460800: case 921600:
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}
esp_err_t ryz_monitor_uart_begin(const ryz_monitor_config_t *config)
{
    esp_err_t error = ryz_monitor_uart_end();
    if (error != ESP_OK) return error;
    error = ryz_monitor_validate_config(config);
    if (error != ESP_OK || config->source != RYZ_MONITOR_UART) return ESP_ERR_INVALID_ARG;
    if (!owner_valid()) return ESP_ERR_INVALID_STATE;
    error = ryz_tool_pins_claim_pair(config->rx, config->tx, &s_lease);
    if (error != ESP_OK) return error;
    s_owner = xTaskGetCurrentTaskHandle();
    s_rx = config->rx;
    error = ryz_tool_pins_claim_controller(RYZ_TOOL_CONTROLLER_UART1, &s_controller_lease);
    if (error != ESP_OK) {
        const esp_err_t cleanup = ryz_monitor_uart_end();
        return cleanup == ESP_OK ? error : cleanup;
    }
    /* The controller lease excludes other broker users even on different
     * pins. These SDK observations also reject unbrokered owners; neither is
     * itself a cross-writer lock. uart_set_pin cannot arbitrate ownership. */
    if (esp_gpio_is_reserved((UINT64_C(1) << config->rx) | (UINT64_C(1) << config->tx)) ||
        uart_is_driver_installed(UART_NUM_1)) {
        error = ryz_monitor_uart_end();
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }
    s_queue = NULL;
    error = uart_driver_install(UART_NUM_1, 2048, 0, 16, &s_queue, 0);
    if (error != ESP_OK) {
        /* 5.5.4 publishes the queue before ISR allocation. Its error cleanup
         * deletes that queue; retaining the early out pointer would be UAF. */
        s_queue = NULL;
        return error;
    }
    s_installed = true;
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    const uart_config_t params = {
        .baud_rate = (int)config->baud, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB,
    };
    error = uart_param_config(UART_NUM_1, &params);
    if (error != ESP_OK) return error;
    uint32_t actual_baud = 0;
    error = uart_get_baudrate(UART_NUM_1, &actual_baud);
    if (error != ESP_OK) return error;
    const uint32_t difference = actual_baud > config->baud ?
        actual_baud - config->baud : config->baud - actual_baud;
    /* Read back the SDK divider, not a claim of physical wire measurement. */
    if ((uint64_t)difference * 100 > (uint64_t)config->baud * 2) return ESP_ERR_NOT_SUPPORTED;
    /* IDF's default interrupt mask omits frame errors. */
    error = uart_enable_intr_mask(UART_NUM_1, UART_INTR_FRAM_ERR);
    if (error != ESP_OK) return error;
    /* RX-only. TX is leased against I2C/other tools but is NEVER driven or
     * remuxed here. GPIO input is configured explicitly: matrix set_pin does
     * not disable a prior GPIO output. Only our modified RX needs restoring. */
    s_restore_rx = true;
    const gpio_config_t input = {
        .pin_bit_mask = UINT64_C(1) << s_rx, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&input);
    if (error != ESP_OK) return error;
    /* Install/configuration kept RX connected to constant-high until here. */
    error = uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, s_rx,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error == ESP_OK) s_active = true;
    return error;
}
esp_err_t ryz_monitor_uart_poll(ryz_monitor_sample_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_active || !s_installed || !s_queue || !owner_valid()) return ESP_ERR_INVALID_STATE;
    /* These are observed notifications only: the SDK can drop queue events,
     * and resets the FIFO before reporting overflow. Never derive lost byte
     * counts from event.size or flush away otherwise readable buffered data. */
    for (unsigned i = 0; i < 8; ++i) {
        uart_event_t event;
        if (xQueueReceive(s_queue, &event, 0) != pdTRUE) break;
        switch (event.type) {
        case UART_FIFO_OVF: ++out->fifo_overflow; break;
        case UART_BUFFER_FULL: ++out->buffer_full; break;
        case UART_FRAME_ERR: ++out->frame_error; break;
        case UART_PARITY_ERR: ++out->parity_error; break;
        case UART_BREAK: ++out->break_events; break;
        default: break;
        }
    }
    /* DATA events may themselves be lost. Always attempt one bounded read,
     * including after BUFFER_FULL so IDF can re-admit its stashed RX bytes. */
    const int length = uart_read_bytes(UART_NUM_1, out->bytes, sizeof(out->bytes), 0);
    if (length < 0 || (size_t)length > sizeof(out->bytes)) {
        memset(out, 0, sizeof(*out));
        return ESP_FAIL;
    }
    out->length = (size_t)length;
    return ESP_OK;
}
esp_err_t ryz_monitor_uart_end(void)
{
    if (ryz_monitor_uart_resources_held() && !owner_valid()) return ESP_ERR_INVALID_STATE;
    s_active = false;
    esp_err_t error;
    if (s_installed) {
        /* 5.5.4 ignores esp_intr_free failure then consumes its objects. A
         * pinned creating CPU avoids the fallible cross-core IPC path. This
         * does not promise recovery from arbitrary SDK internal corruption. */
        error = uart_driver_delete(UART_NUM_1);
        s_queue = NULL;
        s_installed = error != ESP_OK && uart_is_driver_installed(UART_NUM_1);
        if (error != ESP_OK) return error;
    }
    if (s_restore_rx) {
        error = gpio_reset_pin(s_rx);
        if (error != ESP_OK) return error;
        s_restore_rx = false;
    }
    if (s_lease) {
        error = ryz_tool_pins_release(s_lease);
        if (error != ESP_OK) return error;
        s_lease = 0;
    }
    if (s_controller_lease) {
        error = ryz_tool_pins_release_controller(s_controller_lease);
        if (error != ESP_OK) return error;
        s_controller_lease = 0;
    }
    s_owner = NULL;
    return ESP_OK;
}
bool ryz_monitor_uart_resources_held(void)
{
    return s_installed || s_restore_rx || s_lease || s_controller_lease;
}
