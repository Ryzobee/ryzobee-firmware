#include "ryz_peripheral.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include "board_i2c.h"
#include "ryz_tool_pins.h"
#include "ryz_log.h"
#include "ryz_rgb.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/uart_ll.h"
#include "i2c_private.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "Generic peripherals require the audited ESP32-S3 / ESP-IDF 5.5.4"
#endif
#if CONFIG_PM_ENABLE || CONFIG_FREERTOS_SMP || CONFIG_FREERTOS_UNICORE
#error "Peripheral teardown requires pinned dual-core, non-SMP, non-PM tasks"
#endif

/* No worker or ISR runs Lua. The job task owns the SDK handles; coroutines
 * share that owner. A returned timeout never permits an in-flight SPI DMA
 * descriptor or buffer to be reclaimed. Only SPI's hardware-accessed buffers
 * are internal RAM; the bounded session and I2C scratch space live in PSRAM. */
typedef struct {
    spi_transaction_t transaction;
    uint8_t tx[RYZ_PERIPHERAL_BUFFER_MAX] __attribute__((aligned(4)));
    uint8_t rx[RYZ_PERIPHERAL_BUFFER_MAX] __attribute__((aligned(4)));
} spi_transfer_t;

typedef struct {
    uint32_t id, pin_lease, controller_lease;
    ryz_peripheral_kind_t kind;
    int pins[RYZ_TOOL_PINS_MAX];
    unsigned pin_count;
    bool restore_pins, active;
    union {
        struct { unsigned mode; } gpio;
        struct { uint64_t next_us, period_us; bool periodic; } timer;
        struct { unsigned index, resolution; bool configured, timer_configured; } pwm;
        struct {
            i2c_master_bus_handle_t bus;
            i2c_master_dev_handle_t device;
            i2c_master_dev_handle_t data_device;
            bool board;
            uint8_t tx[RYZ_PERIPHERAL_BUFFER_MAX], rx[RYZ_PERIPHERAL_BUFFER_MAX];
            uint8_t probe_address[2];
        } i2c;
        struct {
            spi_device_handle_t device;
            spi_transfer_t *transfer;
            bool installed, pending;
            bool tx_enabled, rx_enabled;
        } spi;
        struct {
            uart_port_t port;
            QueueHandle_t events;
            bool installed, tx_enabled, rx_enabled;
            uint32_t dropped;
        } uart;
        struct {
            adc_unit_t unit;
            adc_channel_t channel;
            adc_cali_handle_t calibration;
            bool referenced;
        } adc;
        struct { void *subscription; } log;
        struct { uint32_t lease; } led;
    } u;
} peripheral_slot_t;

typedef struct peripheral_state {
    TaskHandle_t owner;
    peripheral_slot_t slots[RYZ_PERIPHERAL_HANDLE_MAX];
    adc_oneshot_unit_handle_t adc_units[SOC_ADC_PERIPH_NUM];
    uint32_t adc_leases[SOC_ADC_PERIPH_NUM];
    unsigned adc_references[SOC_ADC_PERIPH_NUM];
    struct peripheral_state *quarantine_next;
} peripheral_state_t;

static atomic_uint s_last_handle;
/* Firmware normally runs one Lua job. Cap admission independently so even
 * unexpected concurrent callers cannot turn failed cleanup into an
 * unbounded PSRAM leak. Quarantine also prevents any further OPEN. */
#define PERIPHERAL_SESSION_MAX 4U
static atomic_uint s_sessions;
static portMUX_TYPE s_quarantine_lock = portMUX_INITIALIZER_UNLOCKED;
static peripheral_state_t *s_quarantined;
static atomic_bool s_quarantine_stop;

static ryz_peripheral_result_t result_from_esp(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return RYZ_PERIPHERAL_OK;
    case ESP_ERR_INVALID_ARG: return RYZ_PERIPHERAL_INVALID;
    case ESP_ERR_NO_MEM: return RYZ_PERIPHERAL_NO_MEMORY;
    case ESP_ERR_TIMEOUT: return RYZ_PERIPHERAL_TIMEOUT;
    case ESP_ERR_NOT_FOUND: return RYZ_PERIPHERAL_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return RYZ_PERIPHERAL_UNSUPPORTED;
    case ESP_ERR_NOT_FINISHED: return RYZ_PERIPHERAL_BUSY;
    case ESP_ERR_INVALID_STATE: return RYZ_PERIPHERAL_BUSY;
    default: return RYZ_PERIPHERAL_FAILED;
    }
}

static bool owner_valid(const peripheral_state_t *state)
{
    /* Pinned creation/destruction avoids the SDK's fallible cross-core
     * interrupt-free IPC path. An unbound task is not a valid owner even
     * when it happens to be executing on core 1 at this instant. */
    return state && state->owner == xTaskGetCurrentTaskHandle() &&
        xTaskGetCoreID(NULL) == 1 && xPortGetCoreID() == 1;
}

static TickType_t wait_ticks(unsigned milliseconds)
{
    /* Floor, not ceil: never lengthen the caller's SDK wait above 20 ms.
     * This is an individual driver-wait bound, not a wall-clock scheduler
     * latency guarantee for the complete Lua function. */
    return pdMS_TO_TICKS(milliseconds);
}

static void add_saturated(uint32_t *value, uint32_t amount)
{
    *value = UINT32_MAX - *value < amount ? UINT32_MAX : *value + amount;
}

static esp_err_t claim_pins(peripheral_slot_t *slot, const int *pins, unsigned count)
{
    esp_err_t error = ryz_tool_pins_claim(pins, count, &slot->pin_lease);
    if (error != ESP_OK) return error;
    uint64_t mask = 0;
    slot->pin_count = count;
    memcpy(slot->pins, pins, count * sizeof(*pins));
    for (unsigned i = 0; i < count; ++i) mask |= UINT64_C(1) << pins[i];
    /* This additional SDK check is not the ownership lock: the broker was
     * acquired first, before any driver can remux a pin. */
    return esp_gpio_is_reserved(mask) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t close_adc_unit(peripheral_state_t *state, adc_unit_t unit)
{
    if (state->adc_references[unit]) return ESP_OK;
    esp_err_t error;
    if (state->adc_units[unit]) {
        error = adc_oneshot_del_unit(state->adc_units[unit]);
        if (error != ESP_OK) return error;
        state->adc_units[unit] = NULL;
    }
    if (state->adc_leases[unit]) {
        error = ryz_tool_pins_release_controller(state->adc_leases[unit]);
        if (error != ESP_OK) return error;
        state->adc_leases[unit] = 0;
    }
    return ESP_OK;
}

static esp_err_t finish_spi(peripheral_slot_t *slot, unsigned timeout_ms)
{
    if (!slot->u.spi.pending) return ESP_OK;
    spi_transaction_t *completed = NULL;
    esp_err_t error = spi_device_get_trans_result(slot->u.spi.device, &completed,
                                                  wait_ticks(timeout_ms));
    if (completed == &slot->u.spi.transfer->transaction) {
        /* IDF also returns this pointer when completed DMA reports an error.
         * The completion queue was consumed, so there is no pending DMA to
         * wait for again; retain the failure, not an eternal phantom wait. */
        slot->u.spi.pending = false;
        return error == ESP_OK ? ESP_OK : ESP_FAIL;
    }
    return error != ESP_OK ? error : ESP_ERR_INVALID_STATE;
}

static esp_err_t log_open(peripheral_slot_t *slot, unsigned level)
{
    ryz_log_subscription_t *subscription = NULL;
    esp_err_t error = ryz_log_subscribe(level, &subscription);
    if (error == ESP_OK) slot->u.log.subscription = subscription;
    return error;
}

static esp_err_t log_close(peripheral_slot_t *slot)
{
    if (!slot->u.log.subscription) return ESP_OK;
    esp_err_t error = ryz_log_unsubscribe(slot->u.log.subscription);
    if (error == ESP_OK) slot->u.log.subscription = NULL;
    return error;
}

static esp_err_t close_led(peripheral_slot_t *slot)
{
    if (!slot->u.led.lease) return ESP_OK;
    int64_t deadline = esp_timer_get_time() + RYZ_PERIPHERAL_WAIT_MAX_MS * 1000;
    esp_err_t error = ryz_rgb_lease_close(slot->u.led.lease);
    if (error != ESP_OK && error != ESP_ERR_NOT_FINISHED) {
        ryz_rgb_snapshot_t status;
        if (ryz_rgb_lease_snapshot(slot->u.led.lease, &status) == ESP_OK &&
            status.phase == RYZ_RGB_FAILED) {
            /* close above distinguishes a cached failed cleanup from a failed
             * write (which it moves to CLEANING). Retry that same lease once
             * per caller request, never repeatedly inside the polling loop. */
            error = ryz_rgb_lease_retry_close(slot->u.led.lease);
            if (error == ESP_OK) error = ryz_rgb_lease_close(slot->u.led.lease);
        }
    }
    while (error == ESP_ERR_NOT_FINISHED && esp_timer_get_time() < deadline) {
        vTaskDelay(1); /* Let the lower-priority, core-1 RGB owner make progress. */
        error = ryz_rgb_lease_close(slot->u.led.lease);
    }
    if (error == ESP_OK) slot->u.led.lease = 0;
    return error;
}

static ryz_peripheral_result_t log_read(peripheral_slot_t *slot,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply)
{
    return result_from_esp(ryz_log_read(slot->u.log.subscription, reply->data,
        request->read_length, &reply->length, &reply->dropped, &reply->loss_possible));
}

static ryz_peripheral_result_t log_write(const ryz_peripheral_request_t *request,
    ryz_peripheral_reply_t *reply)
{
    esp_err_t error = ryz_log_write(request->config.log.level, request->data, request->length);
    if (error == ESP_OK) reply->length = request->length;
    return result_from_esp(error);
}

static esp_err_t close_slot(peripheral_state_t *state, peripheral_slot_t *slot)
{
    /* Revoke calls immediately, including after partial-open failures. Keep
     * each ownership token until its SDK resource and pin mux are torn down. */
    slot->active = false;
    esp_err_t error;
    switch (slot->kind) {
    case RYZ_PERIPHERAL_PWM:
        if (slot->u.pwm.configured) {
            error = ledc_stop(LEDC_LOW_SPEED_MODE, slot->u.pwm.index, 0);
            if (error != ESP_OK) return error;
            slot->u.pwm.configured = false;
        }
        if (slot->u.pwm.timer_configured) {
            error = ledc_timer_pause(LEDC_LOW_SPEED_MODE, slot->u.pwm.index);
            if (error != ESP_OK) return error;
            const ledc_timer_config_t timer = {
                .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = slot->u.pwm.index,
                .deconfigure = true,
            };
            error = ledc_timer_config(&timer);
            if (error != ESP_OK) return error;
            slot->u.pwm.timer_configured = false;
        }
        break;
    case RYZ_PERIPHERAL_I2C:
        if (slot->u.i2c.data_device) {
            error = i2c_master_bus_rm_device(slot->u.i2c.data_device);
            if (error != ESP_OK) return error;
            slot->u.i2c.data_device = NULL;
        }
        if (slot->u.i2c.device) {
            error = i2c_master_bus_rm_device(slot->u.i2c.device);
            if (error != ESP_OK) return error;
            slot->u.i2c.device = NULL;
        }
        if (slot->u.i2c.bus) {
            error = i2c_del_master_bus(slot->u.i2c.bus);
            if (error != ESP_OK) return error;
            slot->u.i2c.bus = NULL;
        }
        break;
    case RYZ_PERIPHERAL_SPI:
        error = finish_spi(slot, RYZ_PERIPHERAL_WAIT_MAX_MS);
        if (error != ESP_OK) return error;
        if (slot->u.spi.device) {
            error = spi_bus_remove_device(slot->u.spi.device);
            if (error != ESP_OK) return error;
            slot->u.spi.device = NULL;
        }
        if (slot->u.spi.installed) {
            error = spi_bus_free(SPI3_HOST);
            if (error != ESP_OK) return error;
            slot->u.spi.installed = false;
        }
        heap_caps_free(slot->u.spi.transfer);
        slot->u.spi.transfer = NULL;
        break;
    case RYZ_PERIPHERAL_UART:
        if (slot->u.uart.installed) {
            error = uart_driver_delete(slot->u.uart.port);
            slot->u.uart.events = NULL;
            if (error != ESP_OK) return error;
            slot->u.uart.installed = false;
        }
        break;
    case RYZ_PERIPHERAL_ADC:
        if (slot->u.adc.calibration) {
            error = adc_cali_delete_scheme_curve_fitting(slot->u.adc.calibration);
            if (error != ESP_OK) return error;
            slot->u.adc.calibration = NULL;
        }
        if (slot->u.adc.referenced) {
            --state->adc_references[slot->u.adc.unit];
            slot->u.adc.referenced = false;
        }
        error = close_adc_unit(state, slot->u.adc.unit);
        if (error != ESP_OK) return error;
        break;
    case RYZ_PERIPHERAL_LOG:
        error = log_close(slot);
        if (error != ESP_OK) return error;
        break;
    case RYZ_PERIPHERAL_LED:
        error = close_led(slot);
        if (error != ESP_OK) return error;
        break;
    default: break;
    }
    if (slot->restore_pins) {
        for (unsigned i = 0; i < slot->pin_count; ++i) {
            error = gpio_reset_pin(slot->pins[i]);
            if (error != ESP_OK) return error;
        }
        slot->restore_pins = false;
    }
    if (slot->pin_lease) {
        error = ryz_tool_pins_release(slot->pin_lease);
        if (error != ESP_OK) return error;
        slot->pin_lease = 0;
    }
    if (slot->controller_lease) {
        error = ryz_tool_pins_release_controller(slot->controller_lease);
        if (error != ESP_OK) return error;
        slot->controller_lease = 0;
    }
    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

static esp_err_t open_gpio(peripheral_slot_t *slot, const ryz_peripheral_config_t *config)
{
    if (config->gpio.mode > 2 || config->gpio.pull > 3 || config->gpio.initial > 1)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = claim_pins(slot, &config->gpio.pin, 1);
    if (error != ESP_OK) return error;
    slot->restore_pins = true;
    slot->u.gpio.mode = config->gpio.mode;
    /* Set the output latch before enabling the driver, avoiding a transient
     * low/high from an inherited latch when opening an output. */
    if (config->gpio.mode) {
        error = gpio_set_level(config->gpio.pin, config->gpio.initial);
        if (error != ESP_OK) return error;
    }
    const gpio_config_t gpio = {
        .pin_bit_mask = UINT64_C(1) << config->gpio.pin,
        .mode = config->gpio.mode == 0 ? GPIO_MODE_INPUT :
                config->gpio.mode == 1 ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = (config->gpio.pull & 1) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (config->gpio.pull & 2) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&gpio);
}

static esp_err_t open_timer(peripheral_slot_t *slot, const ryz_peripheral_config_t *config)
{
    if (!config->timer.period_ms || config->timer.period_ms > 3600000)
        return ESP_ERR_INVALID_ARG;
    slot->u.timer.period_us = (uint64_t)config->timer.period_ms * 1000;
    slot->u.timer.next_us = esp_timer_get_time() + slot->u.timer.period_us;
    slot->u.timer.periodic = config->timer.periodic;
    return ESP_OK;
}

static esp_err_t set_pwm(peripheral_slot_t *slot, unsigned duty)
{
    if (duty == 0 || duty == 1000)
        return ledc_stop(LEDC_LOW_SPEED_MODE, slot->u.pwm.index, duty == 1000);
    const uint32_t steps = UINT32_C(1) << slot->u.pwm.resolution;
    uint32_t quantized = (duty * steps + 500U) / 1000U;
    if (quantized >= steps) quantized = steps - 1;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, slot->u.pwm.index, quantized);
    if (error == ESP_OK) error = ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->u.pwm.index);
    return error;
}

static esp_err_t open_pwm(peripheral_slot_t *slot, const ryz_peripheral_config_t *config)
{
    if (config->pwm.duty > 1000 || !config->pwm.frequency_hz ||
        config->pwm.frequency_hz > 1000000) return ESP_ERR_INVALID_ARG;
    slot->u.pwm.resolution = ledc_find_suitable_duty_resolution(esp_clk_apb_freq(),
                                                               config->pwm.frequency_hz);
    if (!slot->u.pwm.resolution) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    for (unsigned index = 1; index <= 3; ++index) {
        error = ryz_tool_pins_claim_controller(RYZ_TOOL_CONTROLLER_LEDC_TIMER1 + index - 1,
                                               &slot->controller_lease);
        if (error == ESP_OK) { slot->u.pwm.index = index; break; }
        if (error != ESP_ERR_INVALID_STATE && error != ESP_ERR_NOT_FINISHED) return error;
    }
    if (error != ESP_OK) return error;
    error = claim_pins(slot, &config->pwm.pin, 1);
    if (error != ESP_OK) return error;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = slot->u.pwm.resolution,
        .timer_num = slot->u.pwm.index, .freq_hz = config->pwm.frequency_hz,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    error = ledc_timer_config(&timer);
    if (error != ESP_OK) return error;
    slot->u.pwm.timer_configured = true;
    uint32_t actual = ledc_get_freq(LEDC_LOW_SPEED_MODE, slot->u.pwm.index);
    uint32_t difference = actual > config->pwm.frequency_hz ?
        actual - config->pwm.frequency_hz : config->pwm.frequency_hz - actual;
    if (!actual || (uint64_t)difference * 100 > (uint64_t)config->pwm.frequency_hz * 2)
        return ESP_ERR_NOT_SUPPORTED;
    slot->restore_pins = true;
    const ledc_channel_config_t channel = {
        .gpio_num = config->pwm.pin, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = slot->u.pwm.index, .timer_sel = slot->u.pwm.index,
        .duty = 0,
        .hpoint = 0, .intr_type = LEDC_INTR_DISABLE,
    };
    /* A failing channel_config can have touched the mux/channel already. */
    slot->u.pwm.configured = true;
    error = ledc_channel_config(&channel);
    return error == ESP_OK ? set_pwm(slot, config->pwm.duty) : error;
}

static esp_err_t open_i2c(peripheral_slot_t *slot, const ryz_peripheral_config_t *config)
{
    slot->u.i2c.board = config->i2c.board;
    if (config->i2c.board) {
        if (config->i2c.frequency_hz != 100000 || config->i2c.sda != -1 ||
            config->i2c.scl != -1) return ESP_ERR_INVALID_ARG;
        /* The board service retains ownership of I2C0 for the entire boot.
         * This handle grants address-probe only, never arbitrary registers. */
        return ryz_board_i2c_init();
    }
    if (config->i2c.frequency_hz != 100000 && config->i2c.frequency_hz != 400000)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = ryz_tool_pins_claim_controller(RYZ_TOOL_CONTROLLER_I2C1,
                                                     &slot->controller_lease);
    if (error != ESP_OK) return error;
    const int pins[] = {config->i2c.sda, config->i2c.scl};
    error = claim_pins(slot, pins, 2);
    if (error != ESP_OK) return error;
    if (i2c_bus_occupied(I2C_NUM_1)) return ESP_ERR_INVALID_STATE;
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_1, .sda_io_num = config->i2c.sda, .scl_io_num = config->i2c.scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .trans_queue_depth = 0, .flags.enable_internal_pullup = true,
    };
    slot->restore_pins = true;
    error = i2c_new_master_bus(&bus, &slot->u.i2c.bus);
    if (error != ESP_OK) return error;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = I2C_DEVICE_ADDRESS_NOT_USED,
        .scl_speed_hz = config->i2c.frequency_hz, .scl_wait_us = 3000,
    };
    error = i2c_master_bus_add_device(slot->u.i2c.bus, &device, &slot->u.i2c.device);
    if (error != ESP_OK) return error;
    i2c_device_config_t data_device = device;
    data_device.device_address = 0x08;
    return i2c_master_bus_add_device(slot->u.i2c.bus, &data_device, &slot->u.i2c.data_device);
}

static esp_err_t open_spi(peripheral_slot_t *slot, const ryz_peripheral_config_t *config)
{
    if (config->spi.mode > 3 || config->spi.frequency_hz < 100000 ||
        config->spi.frequency_hz > 20000000 || config->spi.cs < 0 ||
        (config->spi.mosi < 0 && config->spi.miso < 0)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ryz_tool_pins_claim_controller(RYZ_TOOL_CONTROLLER_SPI3,
                                                     &slot->controller_lease);
    if (error != ESP_OK) return error;
    int pins[4] = {config->spi.sclk};
    unsigned count = 1;
    if (config->spi.mosi >= 0) pins[count++] = config->spi.mosi;
    if (config->spi.miso >= 0) pins[count++] = config->spi.miso;
    if (config->spi.cs >= 0) pins[count++] = config->spi.cs;
    error = claim_pins(slot, pins, count);
    if (error != ESP_OK) return error;
    slot->u.spi.transfer = heap_caps_calloc(1, sizeof(spi_transfer_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!slot->u.spi.transfer) return ESP_ERR_NO_MEM;
    const spi_bus_config_t bus = {
        .mosi_io_num = config->spi.mosi, .miso_io_num = config->spi.miso,
        .sclk_io_num = config->spi.sclk, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .data4_io_num = -1, .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = RYZ_PERIPHERAL_BUFFER_MAX,
    };
    slot->restore_pins = true;
    error = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) return error;
    slot->u.spi.installed = true;
    const spi_device_interface_config_t device = {
        .clock_speed_hz = config->spi.frequency_hz, .mode = config->spi.mode,
        .spics_io_num = config->spi.cs, .queue_size = 1,
    };
    slot->u.spi.tx_enabled = config->spi.mosi >= 0;
    slot->u.spi.rx_enabled = config->spi.miso >= 0;
    return spi_bus_add_device(SPI3_HOST, &device, &slot->u.spi.device);
}

static esp_err_t open_uart(peripheral_slot_t *slot, const ryz_peripheral_config_t *config)
{
    if (config->uart.port < 1 || config->uart.port > 2 ||
        config->uart.baud < 300 || config->uart.baud > 5000000 ||
        config->uart.bits < 5 || config->uart.bits > 8 || config->uart.parity > 2 ||
        config->uart.stop < 1 || config->uart.stop > 2 ||
        (config->uart.tx < 0 && config->uart.rx < 0)) return ESP_ERR_INVALID_ARG;
    if (config->uart.port == CONFIG_ESP_CONSOLE_UART_NUM) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t error = ryz_tool_pins_claim_controller(config->uart.port == 1 ?
        RYZ_TOOL_CONTROLLER_UART1 : RYZ_TOOL_CONTROLLER_UART2, &slot->controller_lease);
    if (error != ESP_OK) return error;
    slot->u.uart.port = config->uart.port;
    int pins[2];
    unsigned count = 0;
    if (config->uart.tx >= 0) pins[count++] = config->uart.tx;
    if (config->uart.rx >= 0) pins[count++] = config->uart.rx;
    error = claim_pins(slot, pins, count);
    if (error != ESP_OK) return error;
    if (uart_is_driver_installed(slot->u.uart.port)) return ESP_ERR_INVALID_STATE;
    error = uart_driver_install(slot->u.uart.port, 512, 0, 16, &slot->u.uart.events, 0);
    if (error != ESP_OK) {
        /* IDF can publish this out-pointer before a later failure deletes it. */
        slot->u.uart.events = NULL;
        return error;
    }
    slot->u.uart.installed = true;
    const uart_config_t params = {
        .baud_rate = config->uart.baud, .data_bits = UART_DATA_5_BITS + config->uart.bits - 5,
        .parity = config->uart.parity == 0 ? UART_PARITY_DISABLE :
                  config->uart.parity == 1 ? UART_PARITY_EVEN : UART_PARITY_ODD,
        .stop_bits = config->uart.stop == 1 ? UART_STOP_BITS_1 : UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_APB,
    };
    error = uart_param_config(slot->u.uart.port, &params);
    if (error != ESP_OK) return error;
    uint32_t actual = 0;
    error = uart_get_baudrate(slot->u.uart.port, &actual);
    if (error != ESP_OK) return error;
    uint32_t difference = actual > config->uart.baud ? actual - config->uart.baud : config->uart.baud - actual;
    if ((uint64_t)difference * 100 > (uint64_t)config->uart.baud * 2) return ESP_ERR_NOT_SUPPORTED;
    error = uart_enable_intr_mask(slot->u.uart.port, UART_INTR_FRAM_ERR);
    if (error != ESP_OK) return error;
    slot->restore_pins = true;
    if (config->uart.rx >= 0) {
        const gpio_config_t input = {
            .pin_bit_mask = UINT64_C(1) << config->uart.rx, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        error = gpio_config(&input);
        if (error != ESP_OK) return error;
    }
    slot->u.uart.tx_enabled = config->uart.tx >= 0;
    slot->u.uart.rx_enabled = config->uart.rx >= 0;
    return uart_set_pin(slot->u.uart.port, config->uart.tx, config->uart.rx,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static esp_err_t open_adc(peripheral_state_t *state, peripheral_slot_t *slot,
                           const ryz_peripheral_config_t *config)
{
    adc_atten_t attenuation;
    switch (config->adc.attenuation_db) {
    case 0: attenuation = ADC_ATTEN_DB_0; break;
    case 2: attenuation = ADC_ATTEN_DB_2_5; break;
    case 6: attenuation = ADC_ATTEN_DB_6; break;
    case 12: attenuation = ADC_ATTEN_DB_12; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = adc_oneshot_io_to_channel(config->adc.pin,
        &slot->u.adc.unit, &slot->u.adc.channel);
    if (error != ESP_OK) return error;
    error = claim_pins(slot, &config->adc.pin, 1);
    if (error != ESP_OK) return error;
    const adc_unit_t unit = slot->u.adc.unit;
    if (!state->adc_units[unit]) {
        if (state->adc_leases[unit]) return ESP_ERR_INVALID_STATE;
        error = ryz_tool_pins_claim_controller(unit == ADC_UNIT_1 ?
            RYZ_TOOL_CONTROLLER_ADC1 : RYZ_TOOL_CONTROLLER_ADC2, &state->adc_leases[unit]);
        if (error != ESP_OK) return error;
        const adc_oneshot_unit_init_cfg_t init = {
            .unit_id = unit, .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        error = adc_oneshot_new_unit(&init, &state->adc_units[unit]);
        if (error != ESP_OK) return error;
    }
    ++state->adc_references[unit];
    slot->u.adc.referenced = true;
    slot->restore_pins = true;
    const adc_oneshot_chan_cfg_t channel = {
        .atten = attenuation, .bitwidth = ADC_BITWIDTH_12,
    };
    error = adc_oneshot_config_channel(state->adc_units[unit], slot->u.adc.channel, &channel);
    if (error != ESP_OK) return error;
    const adc_cali_curve_fitting_config_t calibration = {
        .unit_id = unit, .chan = slot->u.adc.channel,
        .atten = attenuation, .bitwidth = ADC_BITWIDTH_12,
    };
    error = adc_cali_create_scheme_curve_fitting(&calibration, &slot->u.adc.calibration);
    /* Raw reads remain available when this chip has no usable calibration.
     * read_mv must report unsupported, never invent a linear voltage. */
    return error == ESP_ERR_NOT_SUPPORTED ? ESP_OK : error;
}

static ryz_peripheral_result_t open_slot(peripheral_state_t *state,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply)
{
    peripheral_slot_t *slot = NULL;
    for (unsigned i = 0; i < RYZ_PERIPHERAL_HANDLE_MAX; ++i)
        if (!state->slots[i].id) { slot = &state->slots[i]; break; }
    if (!slot) return RYZ_PERIPHERAL_NO_MEMORY;
    unsigned previous = atomic_load(&s_last_handle);
    do {
        if (previous >= INT32_MAX) return RYZ_PERIPHERAL_NO_MEMORY;
    } while (!atomic_compare_exchange_weak(&s_last_handle, &previous, previous + 1));
    slot->id = previous + 1;
    slot->kind = request->kind;
    esp_err_t error;
    switch (request->kind) {
    case RYZ_PERIPHERAL_GPIO: error = open_gpio(slot, &request->config); break;
    case RYZ_PERIPHERAL_TIMER: error = open_timer(slot, &request->config); break;
    case RYZ_PERIPHERAL_PWM: error = open_pwm(slot, &request->config); break;
    case RYZ_PERIPHERAL_I2C: error = open_i2c(slot, &request->config); break;
    case RYZ_PERIPHERAL_SPI: error = open_spi(slot, &request->config); break;
    case RYZ_PERIPHERAL_UART: error = open_uart(slot, &request->config); break;
    case RYZ_PERIPHERAL_ADC: error = open_adc(state, slot, &request->config); break;
    case RYZ_PERIPHERAL_LOG: error = log_open(slot, request->config.log.level); break;
    case RYZ_PERIPHERAL_LED:
        error = request->config.led.board ? ryz_rgb_lease_open(&slot->u.led.lease) : ESP_ERR_INVALID_ARG;
        break;
    default: error = ESP_ERR_NOT_SUPPORTED; break;
    }
    if (error != ESP_OK) {
        /* A failed cleanup keeps this non-active slot and its leases. The
         * caller never receives a usable handle for a partial-open device. */
        (void)close_slot(state, slot);
        return result_from_esp(error);
    }
    slot->active = true;
    reply->handle = slot->id;
    return RYZ_PERIPHERAL_OK;
}

static esp_err_t custom_i2c_probe(peripheral_slot_t *slot, uint8_t address,
                                   unsigned timeout_ms)
{
    i2c_master_bus_handle_t bus = slot->u.i2c.bus;
    uint8_t *byte = bus->i2c_ops[1].data == &slot->u.i2c.probe_address[0] ?
        &slot->u.i2c.probe_address[1] : &slot->u.i2c.probe_address[0];
    *byte = address << 1;
    i2c_operation_job_t operations[] = {
        {.command = I2C_MASTER_CMD_START},
        {.command = I2C_MASTER_CMD_WRITE, .write = {.ack_check = true, .data = byte, .total_bytes = 1}},
        {.command = I2C_MASTER_CMD_STOP},
    };
    esp_err_t error = i2c_master_execute_defined_operations(slot->u.i2c.device,
        operations, sizeof(operations) / sizeof(operations[0]), timeout_ms);
    /* Pinned 5.5.4 stores a copy of the operations. The persistent byte's
     * identity plus bytes_used distinguish this transaction's NACK from a
     * stale NACK after a pre-lock/preflight failure. No private state writes. */
    const i2c_operation_t *write = &bus->i2c_ops[1];
    const bool fresh = write->data == byte && write->total_bytes == 1 &&
        write->bytes_used == 1 && write->hw_cmd.op_code == I2C_LL_CMD_WRITE;
    const i2c_master_status_t status = atomic_load(&bus->status);
    if (error == ESP_OK) return fresh && status == I2C_STATUS_DONE ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (error == ESP_ERR_INVALID_STATE && fresh) {
        if (status == I2C_STATUS_ACK_ERROR) return ESP_ERR_NOT_FOUND;
        if (status == I2C_STATUS_TIMEOUT) return ESP_ERR_TIMEOUT;
    }
    return error;
}

static ryz_peripheral_result_t i2c_call(peripheral_slot_t *slot,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply)
{
    if (request->address < 0x08 || request->address > 0x77) return RYZ_PERIPHERAL_INVALID;
    if (request->op == RYZ_PERIPHERAL_PROBE) {
        esp_err_t error = slot->u.i2c.board ? ryz_board_i2c_try_probe(request->address) :
            custom_i2c_probe(slot, request->address, request->timeout_ms);
        reply->value = error == ESP_OK;
        return result_from_esp(error);
    }
    if (slot->u.i2c.board) return RYZ_PERIPHERAL_UNSUPPORTED;
    if (request->op != RYZ_PERIPHERAL_WRITE && request->op != RYZ_PERIPHERAL_READ &&
        request->op != RYZ_PERIPHERAL_TRANSFER) return RYZ_PERIPHERAL_UNSUPPORTED;
    const bool writing = request->op != RYZ_PERIPHERAL_READ;
    const bool reading = request->op != RYZ_PERIPHERAL_WRITE;
    if ((writing && !request->length) || (reading && !request->read_length)) return RYZ_PERIPHERAL_INVALID;
    /* One data SDK device is reused; a separate fixed no-address device is
     * used for probe operations. No unbounded address-to-handle cache. */
    esp_err_t error = i2c_master_device_change_address(slot->u.i2c.data_device,
        request->address, request->timeout_ms);
    if (error != ESP_OK) return result_from_esp(error);
    if (writing) memcpy(slot->u.i2c.tx, request->data, request->length);
    if (request->op == RYZ_PERIPHERAL_TRANSFER) {
        error = i2c_master_transmit_receive(slot->u.i2c.data_device,
            slot->u.i2c.tx, request->length, slot->u.i2c.rx, request->read_length, request->timeout_ms);
    } else if (reading) {
        error = i2c_master_receive(slot->u.i2c.data_device, slot->u.i2c.rx,
                                    request->read_length, request->timeout_ms);
    } else {
        error = i2c_master_transmit(slot->u.i2c.data_device, slot->u.i2c.tx,
                                    request->length, request->timeout_ms);
    }
    if (error == ESP_OK) {
        if (reading) {
            reply->length = request->read_length;
            memcpy(reply->data, slot->u.i2c.rx, reply->length);
        } else reply->length = request->length;
    }
    return result_from_esp(error);
}

static ryz_peripheral_result_t spi_call(peripheral_slot_t *slot,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply)
{
    if (request->op != RYZ_PERIPHERAL_TRANSFER && request->op != RYZ_PERIPHERAL_READ &&
        request->op != RYZ_PERIPHERAL_WRITE) return RYZ_PERIPHERAL_UNSUPPORTED;
    /* A previous timed-out transaction may still be on the wire. Never
     * enqueue a replacement while its DMA owns the persistent buffer. */
    esp_err_t error = finish_spi(slot, 0);
    if (error != ESP_OK) return error == ESP_ERR_TIMEOUT ? RYZ_PERIPHERAL_BUSY : result_from_esp(error);
    const bool writing = request->op != RYZ_PERIPHERAL_READ;
    const bool reading = request->op != RYZ_PERIPHERAL_WRITE;
    if ((writing && !slot->u.spi.tx_enabled) || (reading && !slot->u.spi.rx_enabled))
        return RYZ_PERIPHERAL_UNSUPPORTED;
    const size_t length = writing ? request->length : request->read_length;
    if (!length || (reading && writing && request->read_length != length)) return RYZ_PERIPHERAL_INVALID;
    spi_transfer_t *transfer = slot->u.spi.transfer;
    memset(&transfer->transaction, 0, sizeof(transfer->transaction));
    if (writing) memcpy(transfer->tx, request->data, length);
    else memset(transfer->tx, 0, length);
    memset(transfer->rx, 0, length);
    transfer->transaction.length = length * 8;
    transfer->transaction.rxlength = reading ? length * 8 : 0;
    transfer->transaction.tx_buffer = slot->u.spi.tx_enabled ? transfer->tx : NULL;
    transfer->transaction.rx_buffer = reading ? transfer->rx : NULL;
    error = spi_device_queue_trans(slot->u.spi.device, &transfer->transaction, 0);
    if (error != ESP_OK) return result_from_esp(error);
    slot->u.spi.pending = true;
    error = finish_spi(slot, request->timeout_ms);
    if (error == ESP_OK) {
        if (reading) {
            reply->length = length;
            memcpy(reply->data, transfer->rx, length);
        } else reply->length = length;
    }
    return result_from_esp(error);
}

static ryz_peripheral_result_t uart_call(peripheral_slot_t *slot,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply)
{
    if (request->op == RYZ_PERIPHERAL_WRITE) {
        if (!slot->u.uart.tx_enabled) return RYZ_PERIPHERAL_UNSUPPORTED;
        if (!request->length) return RYZ_PERIPHERAL_OK;
        /* FIFO-only, partial acceptance is explicit. UART's internal TX
         * mutex is uncontended because this owner is its sole API writer;
         * no uart_write_bytes/portMAX_DELAY FIFO drain is used. */
        int written = uart_tx_chars(slot->u.uart.port, (const char *)request->data, request->length);
        if (written < 0) return RYZ_PERIPHERAL_FAILED;
        reply->length = written;
        return RYZ_PERIPHERAL_OK;
    }
    if (request->op != RYZ_PERIPHERAL_READ) return RYZ_PERIPHERAL_UNSUPPORTED;
    if (!slot->u.uart.rx_enabled) return RYZ_PERIPHERAL_UNSUPPORTED;
    if (!request->read_length) return RYZ_PERIPHERAL_INVALID;
    /* Overflow notifications are counts of observed events, not fabricated
     * lost-byte estimates. SDK queue saturation can lose notifications. */
    for (unsigned i = 0; i < 16 && slot->u.uart.events; ++i) {
        uart_event_t event;
        if (xQueueReceive(slot->u.uart.events, &event, 0) != pdTRUE) break;
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR)
            add_saturated(&slot->u.uart.dropped, 1);
    }
    int length = uart_read_bytes(slot->u.uart.port, reply->data,
                                  request->read_length, wait_ticks(request->timeout_ms));
    if (length < 0 || (size_t)length > request->read_length) return RYZ_PERIPHERAL_FAILED;
    reply->length = length;
    reply->error_events = slot->u.uart.dropped;
    /* Even without an observed overflow, SDK event admission can lose
     * notifications. Zero does not prove the physical input is lossless. */
    reply->loss_possible = true;
    return RYZ_PERIPHERAL_OK;
}

ryz_peripheral_result_t ryz_peripheral_esp_call(void *context,
    const ryz_peripheral_request_t *request, ryz_peripheral_reply_t *reply)
{
    ryz_peripheral_session_t *session = context;
    if (!session || !request || !reply) return RYZ_PERIPHERAL_INVALID;
    memset(reply, 0, sizeof(*reply));
    if ((unsigned)request->kind >= RYZ_PERIPHERAL_KIND_COUNT ||
        request->length > RYZ_PERIPHERAL_BUFFER_MAX || request->read_length > RYZ_PERIPHERAL_BUFFER_MAX ||
        (request->length && !request->data) || request->timeout_ms > RYZ_PERIPHERAL_WAIT_MAX_MS)
        return RYZ_PERIPHERAL_INVALID;
    if (request->kind == RYZ_PERIPHERAL_LOG && request->op == RYZ_PERIPHERAL_WRITE && !request->handle)
        return log_write(request, reply);
    if (request->op == RYZ_PERIPHERAL_OPEN && atomic_load(&s_quarantine_stop))
        return RYZ_PERIPHERAL_UNAVAILABLE;
    if (!session->state) {
        if (request->op != RYZ_PERIPHERAL_OPEN) return RYZ_PERIPHERAL_CLOSED;
        if (xTaskGetCoreID(NULL) != 1 || xPortGetCoreID() != 1)
            return RYZ_PERIPHERAL_UNAVAILABLE;
        unsigned active = atomic_load(&s_sessions);
        do {
            if (active >= PERIPHERAL_SESSION_MAX) return RYZ_PERIPHERAL_NO_MEMORY;
        } while (!atomic_compare_exchange_weak(&s_sessions, &active, active + 1));
        peripheral_state_t *state = heap_caps_calloc(1, sizeof(*state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!state) {
            atomic_fetch_sub(&s_sessions, 1);
            return RYZ_PERIPHERAL_NO_MEMORY;
        }
        state->owner = xTaskGetCurrentTaskHandle();
        session->state = state;
    }
    peripheral_state_t *state = session->state;
    if (!owner_valid(state)) return RYZ_PERIPHERAL_UNAVAILABLE;
    if (request->op == RYZ_PERIPHERAL_OPEN) return open_slot(state, request, reply);
    peripheral_slot_t *slot = NULL;
    for (unsigned i = 0; i < RYZ_PERIPHERAL_HANDLE_MAX; ++i) {
        if (state->slots[i].id == request->handle && state->slots[i].kind == request->kind) {
            slot = &state->slots[i]; break;
        }
    }
    if (!slot || !request->handle) return RYZ_PERIPHERAL_CLOSED;
    if (request->op == RYZ_PERIPHERAL_CLOSE) return result_from_esp(close_slot(state, slot));
    if (!slot->active) return RYZ_PERIPHERAL_CLOSED;
    switch (slot->kind) {
    case RYZ_PERIPHERAL_GPIO:
        if (request->op == RYZ_PERIPHERAL_READ) {
            reply->value = gpio_get_level(slot->pins[0]);
            return RYZ_PERIPHERAL_OK;
        }
        if (request->op == RYZ_PERIPHERAL_WRITE) {
            if (!slot->u.gpio.mode) return RYZ_PERIPHERAL_UNSUPPORTED;
            if (request->value > 1) return RYZ_PERIPHERAL_INVALID;
            return result_from_esp(gpio_set_level(slot->pins[0], request->value));
        }
        break;
    case RYZ_PERIPHERAL_TIMER:
        if (request->op == RYZ_PERIPHERAL_POLL) {
            uint64_t now = esp_timer_get_time();
            if (slot->u.timer.next_us && now >= slot->u.timer.next_us) {
                uint64_t count = 1;
                if (slot->u.timer.periodic) {
                    count += (now - slot->u.timer.next_us) / slot->u.timer.period_us;
                    slot->u.timer.next_us += count * slot->u.timer.period_us;
                } else slot->u.timer.next_us = 0;
                reply->value = count > INT32_MAX ? INT32_MAX : count;
            }
            return RYZ_PERIPHERAL_OK;
        }
        break;
    case RYZ_PERIPHERAL_PWM:
        if (request->op == RYZ_PERIPHERAL_SET_DUTY) {
            if (request->value > 1000) return RYZ_PERIPHERAL_INVALID;
            return result_from_esp(set_pwm(slot, request->value));
        }
        break;
    case RYZ_PERIPHERAL_I2C: return i2c_call(slot, request, reply);
    case RYZ_PERIPHERAL_SPI: return spi_call(slot, request, reply);
    case RYZ_PERIPHERAL_UART: return uart_call(slot, request, reply);
    case RYZ_PERIPHERAL_ADC:
        if (request->op == RYZ_PERIPHERAL_READ || request->op == RYZ_PERIPHERAL_READ_MV) {
            if (request->op == RYZ_PERIPHERAL_READ_MV && !slot->u.adc.calibration)
                return RYZ_PERIPHERAL_UNSUPPORTED;
            int value = 0;
            esp_err_t error = adc_oneshot_read(state->adc_units[slot->u.adc.unit],
                                               slot->u.adc.channel, &value);
            if (error == ESP_OK && request->op == RYZ_PERIPHERAL_READ_MV)
                error = adc_cali_raw_to_voltage(slot->u.adc.calibration, value, &value);
            if (error == ESP_OK && value >= 0) reply->value = value;
            else if (error == ESP_OK) error = ESP_FAIL;
            return result_from_esp(error);
        }
        break;
    case RYZ_PERIPHERAL_LOG:
        if (request->op == RYZ_PERIPHERAL_READ) return log_read(slot, request, reply);
        break;
    case RYZ_PERIPHERAL_LED:
        if (request->op == RYZ_PERIPHERAL_WRITE) {
            if (request->length != 3) return RYZ_PERIPHERAL_INVALID;
            const ryz_rgb_color_t color = {request->data[0], request->data[1], request->data[2]};
            uint32_t accepted;
            return result_from_esp(ryz_rgb_lease_submit(slot->u.led.lease, color, &accepted));
        }
        if (request->op == RYZ_PERIPHERAL_STATUS) {
            ryz_rgb_snapshot_t status;
            esp_err_t error = ryz_rgb_lease_snapshot(slot->u.led.lease, &status);
            if (error != ESP_OK) return result_from_esp(error);
            switch (status.phase) {
            case RYZ_RGB_IDLE: reply->led.state = RYZ_PERIPHERAL_LED_IDLE; break;
            case RYZ_RGB_COMPLETED: reply->led.state = RYZ_PERIPHERAL_LED_READY; break;
            case RYZ_RGB_FAILED: reply->led.state = RYZ_PERIPHERAL_LED_FAILED; break;
            default: reply->led.state = RYZ_PERIPHERAL_LED_PENDING; break;
            }
            reply->led.red = status.last_completed_color.red;
            reply->led.green = status.last_completed_color.green;
            reply->led.blue = status.last_completed_color.blue;
            reply->led.output_known = status.output_known;
            return RYZ_PERIPHERAL_OK;
        }
        break;
    default: break;
    }
    return RYZ_PERIPHERAL_UNSUPPORTED;
}

void ryz_peripheral_esp_cleanup(ryz_peripheral_session_t *session)
{
    if (!session || !session->state) return;
    peripheral_state_t *state = session->state;
    bool held = false;
    if (owner_valid(state)) {
        for (unsigned i = 0; i < RYZ_PERIPHERAL_HANDLE_MAX; ++i) {
            if (!state->slots[i].id) continue;
            /* A producer may briefly own the log/pin admission lock. Retry
             * teardown only, not the user's I/O operation, before treating
             * a still-retained resource as unrecoverable. */
            esp_err_t error = close_slot(state, &state->slots[i]);
            for (unsigned retry = 0; error != ESP_OK && retry < 2; ++retry) {
                vTaskDelay(1);
                error = close_slot(state, &state->slots[i]);
            }
            if (error == ESP_ERR_NOT_FINISHED && state->slots[i].kind == RYZ_PERIPHERAL_LED) {
                ryz_rgb_snapshot_t status;
                if (ryz_rgb_lease_snapshot(state->slots[i].u.led.lease, &status) == ESP_OK &&
                    status.phase == RYZ_RGB_CLEANING) {
                    /* The boot-lifetime LED worker already owns the cleanup
                     * and retains no job pointer. Revoke only this Lua slot;
                     * its service lease continues rejecting new owners until
                     * the worker really releases hardware. Do not quarantine
                     * every peripheral because that worker was delayed. */
                    memset(&state->slots[i], 0, sizeof(state->slots[i]));
                    error = ESP_OK;
                }
            }
            if (error != ESP_OK) held = true;
        }
        for (unsigned i = 0; i < SOC_ADC_PERIPH_NUM; ++i)
            if (close_adc_unit(state, i) != ESP_OK) held = true;
    } else held = true;
    session->state = NULL;
    if (!held) {
        heap_caps_free(state);
        atomic_fetch_sub(&s_sessions, 1);
        return;
    }
    /* Fail closed: retained DMA, pin and controller ownership are reachable
     * for diagnostics and never handed to another job. Reboot is required
     * after an unrecoverable teardown error; do not hide it as successful
     * cleanup or free a structure still retained by an SDK interrupt. */
    atomic_store(&s_quarantine_stop, true);
    portENTER_CRITICAL(&s_quarantine_lock);
    state->quarantine_next = s_quarantined;
    s_quarantined = state;
    portEXIT_CRITICAL(&s_quarantine_lock);
    ESP_LOGE("ryz-peripheral", "job resources quarantined after cleanup failure");
}
