#include "ryz_monitor_source.h"
#include "ryz_tool_pins.h"
#include "ryz_i2c_scan_bus.h"
#include "board_i2c.h"
#include "i2c_private.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "hal/uart_ll.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uart_event_t events[32]; size_t head, count; } host_queue_t;
static host_queue_t *sdk_queue;
static bool installed, other_owner, configured, frame_enabled, connected;
static int core = 1, pinned = 1;
static uintptr_t task_id = 1;
static uint64_t native_reserved;
static unsigned installs, deletes, pin_changes, gpio_changes, resets, reads, queue_reads;
static int expected_rx = 13, expected_tx = 14;
static uint32_t requested_baud = 115200, actual_baud;
static unsigned install_failure, step_failure, delete_failure, reset_failure;
static uint8_t incoming[512];
static size_t incoming_length, incoming_offset;
static int read_failure;
static bool sdk_i2c_exists;
static unsigned i2c_creates, i2c_reset_remaining;

int xPortGetCoreID(void) { return core; }
int xTaskGetCoreID(TaskHandle_t task) { (void)task; return pinned; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)task_id; }
bool esp_gpio_is_reserved(uint64_t mask) { return (native_reserved & mask) != 0; }
bool uart_is_driver_installed(uart_port_t port) { assert(port == UART_NUM_1); return installed || other_owner; }
static void leased(void)
{
    assert(ryz_tool_pins_check_pair(expected_rx, expected_tx) == ESP_ERR_INVALID_STATE);
}
esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int depth,
                             QueueHandle_t *queue, int flags)
{
    assert(port == UART_NUM_1 && rx > 128 && rx <= 4096 && tx == 0 && depth > 0 && depth <= 32 && flags == 0);
    assert(!installed && !other_owner && core == 1 && pinned == 1);
    leased(); ++installs;
    if (install_failure == 1) { install_failure = 0; return ESP_FAIL; }
    sdk_queue = calloc(1, sizeof(*sdk_queue)); assert(sdk_queue);
    *queue = sdk_queue; installed = true;
    if (install_failure == 2) {
        install_failure = 0;
        free(sdk_queue); sdk_queue = NULL; installed = false;
        return ESP_ERR_NO_MEM; /* SDK failure after its early queue output. */
    }
    return ESP_OK;
}
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
    assert(port == UART_NUM_1 && installed && !connected);
    assert(config->baud_rate == (int)requested_baud && config->data_bits == UART_DATA_8_BITS &&
           config->parity == UART_PARITY_DISABLE && config->stop_bits == UART_STOP_BITS_1 &&
           config->flow_ctrl == UART_HW_FLOWCTRL_DISABLE && config->source_clk == UART_SCLK_APB);
    if (step_failure == 1) { step_failure = 0; return ESP_FAIL; }
    configured = true; return ESP_OK;
}
esp_err_t uart_get_baudrate(uart_port_t port, uint32_t *out)
{
    assert(port == UART_NUM_1 && installed && configured && !connected);
    if (step_failure == 2) { step_failure = 0; return ESP_ERR_TIMEOUT; }
    *out = actual_baud ? actual_baud : requested_baud; return ESP_OK;
}
esp_err_t uart_enable_intr_mask(uart_port_t port, uint32_t mask)
{
    assert(port == UART_NUM_1 && installed && configured && !connected);
    assert(mask & UART_INTR_FRAM_ERR);
    if (step_failure == 3) { step_failure = 0; return ESP_FAIL; }
    frame_enabled = true; return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *config)
{
    leased(); ++gpio_changes;
    assert(config->pin_bit_mask == (UINT64_C(1) << expected_rx));
    assert(config->mode == GPIO_MODE_INPUT && config->pull_up_en == GPIO_PULLUP_ENABLE &&
           config->pull_down_en == GPIO_PULLDOWN_DISABLE && config->intr_type == GPIO_INTR_DISABLE);
    if (step_failure == 4) { step_failure = 0; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    leased(); ++pin_changes;
    assert(port == UART_NUM_1 && installed && configured && frame_enabled);
    assert(rx == expected_rx && tx == -1 && rts == -1 && cts == -1);
    if (step_failure == 5) { step_failure = 0; return ESP_FAIL; }
    connected = true; return ESP_OK;
}
int uart_read_bytes(uart_port_t port, void *out, uint32_t length, TickType_t wait)
{
    assert(port == UART_NUM_1 && installed && connected && wait == 0 && length == 96);
    ++reads;
    if (read_failure) { int result = read_failure; read_failure = 0; return result; }
    size_t amount = incoming_length - incoming_offset;
    if (amount > length) amount = length;
    memcpy(out, incoming + incoming_offset, amount); incoming_offset += amount;
    return (int)amount;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t wait)
{
    assert(queue && queue == sdk_queue && installed && wait == 0);
    ++queue_reads;
    if (sdk_queue->head == sdk_queue->count) return pdFALSE;
    *(uart_event_t *)out = sdk_queue->events[sdk_queue->head++]; return pdTRUE;
}
esp_err_t uart_driver_delete(uart_port_t port)
{
    assert(port == UART_NUM_1 && installed && !other_owner && core == 1 && pinned == 1);
    leased(); ++deletes;
    if (delete_failure == 1) { delete_failure = 0; return ESP_FAIL; }
    free(sdk_queue); sdk_queue = NULL; installed = configured = frame_enabled = connected = false;
    if (delete_failure == 2) { delete_failure = 0; return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    leased(); assert(!installed && !other_owner); ++resets;
    if (i2c_reset_remaining) {
        assert(pin == (i2c_reset_remaining == 2 ? expected_rx : expected_tx));
        --i2c_reset_remaining;
        return ESP_OK;
    }
    assert(pin == expected_rx);
    if (reset_failure) { --reset_failure; return ESP_FAIL; }
    native_reserved &= ~(UINT64_C(1) << pin); return ESP_OK;
}
/* I2C interoperation links BOTH real scan bus and real IDF diagnostic Adapter.
 * Only their SDK/board edges are replaced, as with the UART SDK above. */
bool i2c_bus_occupied(int port) { assert(port == I2C_NUM_1); return sdk_i2c_exists; }
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *out)
{
    leased(); assert(!installed && !sdk_i2c_exists); ++i2c_creates;
    assert(config->i2c_port == I2C_NUM_1 && config->sda_io_num == expected_rx && config->scl_io_num == expected_tx);
    *out = calloc(1, sizeof(**out)); assert(*out); sdk_i2c_exists = true; return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                  i2c_master_dev_handle_t *out)
{
    assert(bus && config->scl_speed_hz == 100000);
    *out = calloc(1, sizeof(**out)); assert(*out); (*out)->master_bus = bus; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(device); free(device); return ESP_OK;
}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    assert(bus && sdk_i2c_exists); leased(); free(bus);
    sdk_i2c_exists = false; i2c_reset_remaining = 2; return ESP_OK;
}
esp_err_t i2c_master_execute_defined_operations(i2c_master_dev_handle_t device,
                                               i2c_operation_job_t *ops, size_t count, int timeout)
{
    (void)device; (void)ops; (void)count; (void)timeout;
    assert(!"this lease interoperation case never probes"); return ESP_FAIL;
}
esp_err_t ryz_board_i2c_init(void) { assert(!"no default shared bus in this test"); return ESP_FAIL; }
esp_err_t ryz_board_i2c_try_probe(uint8_t address)
{
    (void)address; assert(!"no board probe in this test"); return ESP_FAIL;
}
static ryz_monitor_config_t uart_config(void)
{
    return (ryz_monitor_config_t){RYZ_MONITOR_UART, expected_rx, expected_tx, requested_baud};
}
static void lifecycle(void)
{
    ryz_monitor_config_t config = uart_config();
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    assert(ryz_monitor_uart_resources_held() && connected && installs == 1);
    leased();
    assert(ryz_monitor_uart_end() == ESP_OK);
    assert(!ryz_monitor_uart_resources_held() && !installed && deletes == 1 && resets == 1);
    assert(ryz_tool_pins_check_pair(expected_rx, expected_tx) == ESP_OK);
    assert(ryz_monitor_uart_end() == ESP_OK && deletes == 1 && resets == 1);
}
static void reception(void)
{
    ryz_monitor_config_t config = uart_config();
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    for (size_t i = 0; i < 212; ++i) incoming[i] = (uint8_t)i;
    incoming_length = 212;
    ryz_monitor_sample_t sample;
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK && sample.length == 96);
    assert(!memcmp(sample.bytes, incoming, 96) && sample.bytes[0] == 0);
    assert(reads == 1 && queue_reads <= 8);
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK && sample.length == 96);
    assert(!memcmp(sample.bytes, incoming + 96, 96));
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK && sample.length == 20);
    assert(!memcmp(sample.bytes, incoming + 192, 20));
    const uart_event_type_t types[] = {UART_FIFO_OVF, UART_BUFFER_FULL, UART_FRAME_ERR,
        UART_PARITY_ERR, UART_BREAK, UART_DATA, UART_FRAME_ERR, UART_FIFO_OVF, UART_BREAK};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        sdk_queue->events[i] = (uart_event_t){types[i], SIZE_MAX, true};
    }
    sdk_queue->count = 9;
    unsigned before = queue_reads;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK && sample.length == 0);
    assert(queue_reads - before == 8 && sample.fifo_overflow == 2 && sample.buffer_full == 1 &&
           sample.frame_error == 2 && sample.parity_error == 1 && sample.break_events == 1);
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK && sample.length == 0 && sample.break_events == 1);
    assert(!sample.fifo_overflow && !sample.frame_error && !sample.parity_error && !sample.buffer_full);
    assert(ryz_monitor_uart_end() == ESP_OK);
}
static void empty_sample(const ryz_monitor_sample_t *sample)
{
    const ryz_monitor_sample_t zero = {0};
    assert(!memcmp(sample, &zero, sizeof(zero)));
}
static void admission(void)
{
    ryz_monitor_config_t config = uart_config();
    core = 0;
    assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_STATE);
    core = 1; pinned = -1;
    assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_STATE);
    pinned = 1;
    for (int pin = expected_rx; pin <= expected_tx; ++pin) {
        native_reserved = UINT64_C(1) << pin;
        assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_STATE);
        assert(!ryz_monitor_uart_resources_held());
        assert(ryz_tool_pins_check_pair(expected_rx, expected_tx) == ESP_OK);
    }
    native_reserved = 0; other_owner = true;
    assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_STATE);
    assert(other_owner && !ryz_monitor_uart_resources_held());
    other_owner = false;
    uint32_t lease;
    assert(ryz_tool_pins_claim_pair(expected_rx, expected_tx, &lease) == ESP_OK);
    assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_STATE);
    assert(!ryz_monitor_uart_resources_held());
    assert(ryz_tool_pins_release(lease) == ESP_OK);
    assert(installs == 0 && pin_changes == 0 && gpio_changes == 0 && deletes == 0 && resets == 0);
    config = RYZ_MONITOR_DEFAULT_CONFIG;
    assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_ARG);
    assert(ryz_monitor_uart_begin(NULL) == ESP_ERR_INVALID_ARG);
    assert(installs == 0 && !ryz_monitor_uart_resources_held());
}
static void wrong_owner(void)
{
    ryz_monitor_config_t config = uart_config();
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    ryz_monitor_sample_t sample;
    task_id = 2;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_monitor_uart_poll(&sample) == ESP_ERR_INVALID_STATE); empty_sample(&sample);
    assert(ryz_monitor_uart_end() == ESP_ERR_INVALID_STATE);
    assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_STATE);
    task_id = 1; core = 0;
    assert(ryz_monitor_uart_end() == ESP_ERR_INVALID_STATE);
    assert(ryz_monitor_uart_resources_held() && reads == 0 && deletes == 0 && resets == 0);
    core = 1;
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK && sample.length == 0);
    assert(ryz_monitor_uart_end() == ESP_OK);
}
static void install_errors(void)
{
    ryz_monitor_config_t config = uart_config();
    for (unsigned failure = 1; failure <= 2; ++failure) {
        install_failure = failure;
        assert(ryz_monitor_uart_begin(&config) == (failure == 1 ? ESP_FAIL : ESP_ERR_NO_MEM));
        assert(ryz_monitor_uart_resources_held() && !installed);
        ryz_monitor_sample_t sample;
        assert(ryz_monitor_uart_poll(&sample) == ESP_ERR_INVALID_STATE); empty_sample(&sample);
        assert(ryz_monitor_uart_end() == ESP_OK);
        assert(!ryz_monitor_uart_resources_held() && deletes == 0 && reads == 0 && queue_reads == 0);
    }
    assert(resets == 0 && pin_changes == 0 && gpio_changes == 0);
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    assert(ryz_monitor_uart_end() == ESP_OK);
}
static void configuration_errors(void)
{
    ryz_monitor_config_t config = uart_config();
    for (unsigned failure = 1; failure <= 5; ++failure) {
        unsigned before_delete = deletes, before_reset = resets;
        step_failure = failure;
        assert(ryz_monitor_uart_begin(&config) == (failure == 2 ? ESP_ERR_TIMEOUT : ESP_FAIL));
        assert(ryz_monitor_uart_resources_held() && installed);
        ryz_monitor_sample_t sample;
        assert(ryz_monitor_uart_poll(&sample) == ESP_ERR_INVALID_STATE); empty_sample(&sample);
        assert(ryz_monitor_uart_end() == ESP_OK);
        assert(!ryz_monitor_uart_resources_held() && deletes == before_delete + 1);
        assert(resets == before_reset + (failure >= 4));
    }
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    assert(ryz_monitor_uart_end() == ESP_OK);
}
static void baud_accuracy(void)
{
    ryz_monitor_config_t config = uart_config();
    const uint32_t accepted[] = {115200, 117504, 112896};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        actual_baud = accepted[i];
        assert(ryz_monitor_uart_begin(&config) == ESP_OK);
        assert(ryz_monitor_uart_end() == ESP_OK);
    }
    const uint32_t rejected[] = {117505, 112895, UINT32_MAX, 1};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        unsigned before = pin_changes;
        actual_baud = rejected[i];
        assert(ryz_monitor_uart_begin(&config) == ESP_ERR_NOT_SUPPORTED && pin_changes == before);
        assert(ryz_monitor_uart_end() == ESP_OK);
    }
}
static void read_errors(void)
{
    ryz_monitor_config_t config = uart_config();
    ryz_monitor_sample_t sample;
    memset(&sample, 0xa5, sizeof(sample));
    assert(ryz_monitor_uart_poll(&sample) == ESP_ERR_INVALID_STATE); empty_sample(&sample);
    assert(ryz_monitor_uart_poll(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    sdk_queue->events[0] = (uart_event_t){.type = UART_FRAME_ERR}; sdk_queue->count = 1;
    read_failure = -1;
    assert(ryz_monitor_uart_poll(&sample) == ESP_FAIL); empty_sample(&sample);
    read_failure = 97; /* Defensive invalid SDK return, not a real UART trace. */
    assert(ryz_monitor_uart_poll(&sample) == ESP_FAIL); empty_sample(&sample);
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK && sample.length == 0);
    assert(ryz_monitor_uart_end() == ESP_OK);
    assert(ryz_monitor_uart_poll(&sample) == ESP_ERR_INVALID_STATE); empty_sample(&sample);
}
static void cleanup(void)
{
    ryz_monitor_config_t config = uart_config();
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    reset_failure = 2;
    assert(ryz_monitor_uart_end() == ESP_FAIL);
    assert(!installed && ryz_monitor_uart_resources_held() && deletes == 1);
    leased();
    ryz_monitor_sample_t sample;
    assert(ryz_monitor_uart_poll(&sample) == ESP_ERR_INVALID_STATE); empty_sample(&sample);
    assert(ryz_monitor_uart_begin(&config) == ESP_FAIL && installs == 1 && deletes == 1);
    assert(ryz_monitor_uart_begin(&config) == ESP_OK && installs == 2 && deletes == 1);
    assert(ryz_monitor_uart_end() == ESP_OK && deletes == 2);
    assert(!ryz_monitor_uart_resources_held());
}
static void defensive_delete(void)
{
    /* The audited fixed-port/same-core SDK delete returns OK. These two
     * synthetic return states test only defensive stale-queue handling, not
     * a claim that real SDK hidden ISR/free failures are recoverable. */
    ryz_monitor_config_t config = uart_config();
    for (unsigned failure = 1; failure <= 2; ++failure) {
        assert(ryz_monitor_uart_begin(&config) == ESP_OK);
        unsigned before = deletes;
        delete_failure = failure;
        assert(ryz_monitor_uart_end() == ESP_FAIL && ryz_monitor_uart_resources_held());
        ryz_monitor_sample_t sample;
        assert(ryz_monitor_uart_poll(&sample) == ESP_ERR_INVALID_STATE);
        assert(ryz_monitor_uart_end() == ESP_OK);
        assert(!ryz_monitor_uart_resources_held());
        assert(deletes == before + (failure == 1 ? 2 : 1));
    }
}
static void i2c_interoperability(void)
{
    ryz_monitor_config_t config = uart_config();
    ryz_i2c_scan_config_t scan = {.sda = expected_rx, .scl = expected_tx, .hz = 100000};
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    assert(ryz_i2c_scan_bus_begin(&scan) == ESP_ERR_INVALID_STATE);
    assert(!ryz_i2c_scan_bus_resources_held() && i2c_creates == 0);
    ryz_monitor_sample_t sample;
    assert(ryz_monitor_uart_poll(&sample) == ESP_OK);
    assert(ryz_monitor_uart_end() == ESP_OK);
    assert(ryz_i2c_scan_bus_begin(&scan) == ESP_OK);
    assert(ryz_i2c_scan_bus_resources_held() && sdk_i2c_exists);
    assert(ryz_monitor_uart_begin(&config) == ESP_ERR_INVALID_STATE && installs == 1);
    assert(!ryz_monitor_uart_resources_held());
    assert(ryz_i2c_scan_bus_end() == ESP_OK);
    assert(!ryz_i2c_scan_bus_resources_held());
    assert(ryz_monitor_uart_begin(&config) == ESP_OK);
    reset_failure = 1;
    assert(ryz_monitor_uart_end() == ESP_FAIL && ryz_monitor_uart_resources_held());
    assert(ryz_i2c_scan_bus_begin(&scan) == ESP_ERR_INVALID_STATE && i2c_creates == 1);
    assert(ryz_monitor_uart_end() == ESP_OK);
    assert(ryz_i2c_scan_bus_begin(&scan) == ESP_OK && i2c_creates == 2);
    assert(ryz_i2c_scan_bus_end() == ESP_OK);
    assert(ryz_tool_pins_check_pair(expected_rx, expected_tx) == ESP_OK);
}

static void validate(void)
{
    ryz_monitor_config_t config = RYZ_MONITOR_DEFAULT_CONFIG;
    assert(ryz_monitor_validate_config(&config) == ESP_OK);
    assert(ryz_monitor_validate_config(NULL) == ESP_ERR_INVALID_ARG);
    config.rx = 13;
    assert(ryz_monitor_validate_config(&config) == ESP_ERR_INVALID_ARG);
    config = (ryz_monitor_config_t){RYZ_MONITOR_UART, 13, 14, 115200};
    const uint32_t bauds[] = {1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,921600};
    for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); ++i) {
        config.baud = bauds[i];
        assert(ryz_monitor_validate_config(&config) == ESP_OK);
    }
    config.baud = 115201;
    assert(ryz_monitor_validate_config(&config) == ESP_ERR_INVALID_ARG);
    config.baud = 115200;
    config.rx = config.tx;
    assert(ryz_monitor_validate_config(&config) == ESP_ERR_INVALID_ARG);
    config.rx = 43;
    assert(ryz_monitor_validate_config(&config) == ESP_ERR_INVALID_ARG);
    config.rx = 48; config.tx = 47;
    assert(ryz_monitor_validate_config(&config) == ESP_OK);
    config.source = (ryz_monitor_source_t)17;
    assert(ryz_monitor_validate_config(&config) == ESP_ERR_INVALID_ARG);
    assert(!ryz_monitor_uart_resources_held());
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "validate")) validate();
    else if (!strcmp(argv[1], "lifecycle")) lifecycle();
    else if (!strcmp(argv[1], "reception")) reception();
    else if (!strcmp(argv[1], "admission")) admission();
    else if (!strcmp(argv[1], "wrong_owner")) wrong_owner();
    else if (!strcmp(argv[1], "install_errors")) install_errors();
    else if (!strcmp(argv[1], "configuration_errors")) configuration_errors();
    else if (!strcmp(argv[1], "baud_accuracy")) baud_accuracy();
    else if (!strcmp(argv[1], "read_errors")) read_errors();
    else if (!strcmp(argv[1], "cleanup")) cleanup();
    else if (!strcmp(argv[1], "defensive_delete")) defensive_delete();
    else if (!strcmp(argv[1], "i2c_interoperability")) i2c_interoperability();
    else assert(!"unknown case");
    puts("MONITOR_UART_PASS");
    return 0;
}
