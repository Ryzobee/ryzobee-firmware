#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ryz_i2c_scan_bus.h"
#include "ryz_tool_pins.h"
#include "i2c_private.h"

static struct i2c_master_bus_t *live_bus;
static struct i2c_master_dev_t *live_device;
static unsigned bus_creates, bus_deletes, device_creates, device_deletes, transfers, resets;
static unsigned board_inits, board_probes;
static int core = 1, affinity = 1;
static uint32_t expected_hz = 100000;
static bool native_reserved;
static bool native_controller_occupied, other_owner_pins_intact = true;
static unsigned controller_checks;
static esp_err_t new_error, add_error, remove_error, delete_error, reset_error;
static unsigned reset_fail_at;
enum { TX_ACK, TX_NACK, TX_TIMEOUT, TX_PRELOCK, TX_PRECLEAR, TX_LOCK_TIMEOUT };
static int tx_mode;
static uint8_t expected_address = 0x18;
bool esp_gpio_is_reserved(uint64_t mask) { assert(mask == ((UINT64_C(1)<<13)|(UINT64_C(1)<<14))); return native_reserved; }
bool i2c_bus_occupied(int port) { assert(port == 1); ++controller_checks; return native_controller_occupied; }
int xPortGetCoreID(void) { return core; }
int xTaskGetCoreID(void *task) { assert(!task); return affinity; }
esp_err_t gpio_reset_pin(int pin) {
    assert(pin == 13 || pin == 14); ++resets;
    return (!reset_fail_at || resets == reset_fail_at) ? reset_error : ESP_OK;
}
esp_err_t ryz_board_i2c_init(void) { ++board_inits; return ESP_OK; }
esp_err_t ryz_board_i2c_try_probe(uint8_t address) { assert(address == 0x18); ++board_probes; return ESP_OK; }
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *out)
{
    assert(!live_bus && !*out);
    assert(config->i2c_port == 1 && config->sda_io_num == 13 && config->scl_io_num == 14);
    assert(config->trans_queue_depth == 0 && config->flags.enable_internal_pullup);
    assert(ryz_tool_pins_check_pair(13, 14) == ESP_ERR_INVALID_STATE);
    ++bus_creates;
    if (native_controller_occupied) {
        /* IDF 5.5.4 acquire returns the existing base with INVALID_STATE;
         * new_master_bus's error cleanup deinitializes that owner's pins. */
        other_owner_pins_intact = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (new_error) return new_error;
    live_bus = calloc(1, sizeof(*live_bus)); assert(live_bus);
    atomic_init(&live_bus->status, I2C_STATUS_IDLE);
    *out = live_bus; return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config, i2c_master_dev_handle_t *out)
{
    assert(bus == live_bus && !live_device && !*out);
    assert(config->device_address == I2C_DEVICE_ADDRESS_NOT_USED && config->scl_speed_hz == expected_hz);
    assert(!config->flags.disable_ack_check);
    ++device_creates;
    if (add_error) return add_error;
    live_device = calloc(1, sizeof(*live_device)); assert(live_device);
    live_device->master_bus = bus;
    *out = live_device; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    assert(device == live_device); ++device_deletes;
    if (remove_error) return remove_error; /* rejected before consumption */
    free(device); live_device = NULL; return ESP_OK;
}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    assert(bus == live_bus && !live_device); ++bus_deletes;
    if (delete_error) return delete_error; /* rejected before consumption */
    free(bus); live_bus = NULL; return ESP_OK;
}
esp_err_t i2c_master_execute_defined_operations(i2c_master_dev_handle_t device, i2c_operation_job_t *ops, size_t count, int timeout)
{
    assert(device == live_device && count == 3 && timeout == 3);
    assert(ops[0].command == I2C_MASTER_CMD_START && ops[2].command == I2C_MASTER_CMD_STOP);
    assert(ops[1].command == I2C_MASTER_CMD_WRITE && ops[1].write.total_bytes == 1 && ops[1].write.ack_check);
    assert(*ops[1].write.data == (uint8_t)(expected_address << 1));
    ++transfers;
    if (tx_mode == TX_PRELOCK) return ESP_ERR_INVALID_STATE;
    if (tx_mode == TX_LOCK_TIMEOUT) return ESP_ERR_TIMEOUT;
    live_bus->i2c_ops[1] = (i2c_operation_t){.data = ops[1].write.data, .total_bytes = 1,
        .bytes_used = 0, .hw_cmd = {.op_code = I2C_LL_CMD_WRITE}};
    /* Mirrors the pinned SDK: memcpy precedes bus-clear/PM preflight; these
     * can fail before status reset and before WRITE consumes any byte. */
    if (tx_mode == TX_PRECLEAR) return ESP_ERR_INVALID_STATE;
    live_bus->i2c_ops[1].bytes_used = 1;
    atomic_store(&live_bus->status, tx_mode == TX_NACK ? I2C_STATUS_ACK_ERROR :
        tx_mode == TX_TIMEOUT ? I2C_STATUS_TIMEOUT : I2C_STATUS_DONE);
    return tx_mode == TX_ACK ? ESP_OK : ESP_ERR_INVALID_STATE;
}
static void lifecycle(void)
{
    ryz_i2c_scan_config_t config = {13, 14, 100000};
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK);
    assert(ryz_i2c_scan_bus_resources_held());
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_OK && transfers == 1);
    assert(ryz_i2c_scan_bus_end() == ESP_OK && !ryz_i2c_scan_bus_resources_held());
    assert(bus_creates == 1 && bus_deletes == 1 && device_creates == 1 && device_deletes == 1 && resets == 2);
    assert(ryz_tool_pins_check_pair(13,14) == ESP_OK);
}
static void errors(void)
{
    const ryz_i2c_scan_config_t config = {13,14,400000}; expected_hz = 400000;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK);
    tx_mode = TX_NACK;
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_NOT_FOUND);
    tx_mode = TX_PRELOCK;
    /* Repeated pre-copy failures must not re-select the previous live slot. */
    for (int i = 0; i < 8; ++i) assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_INVALID_STATE);
    tx_mode = TX_PRECLEAR;
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_INVALID_STATE);
    tx_mode = TX_TIMEOUT;
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_TIMEOUT);
    tx_mode = TX_PRELOCK;
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_INVALID_STATE);
    tx_mode = TX_PRECLEAR;
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_INVALID_STATE);
    tx_mode = TX_LOCK_TIMEOUT;
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_TIMEOUT);
    tx_mode = TX_ACK;
    for (expected_address = 0x08; expected_address <= 0x77; ++expected_address) {
        assert(ryz_i2c_scan_bus_probe(expected_address) == ESP_OK);
    }
    assert(ryz_i2c_scan_bus_end() == ESP_OK);
}
static void allocation(void)
{
    const ryz_i2c_scan_config_t config = {13,14,100000};
    new_error = ESP_ERR_NO_MEM;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_ERR_NO_MEM);
    assert(ryz_i2c_scan_bus_resources_held() && !live_bus && !live_device);
    assert(ryz_tool_pins_check_pair(13,14) == ESP_ERR_INVALID_STATE);
    reset_error = ESP_FAIL;
    assert(ryz_i2c_scan_bus_end() == ESP_FAIL && ryz_i2c_scan_bus_resources_held());
    reset_error = new_error = ESP_OK; add_error = ESP_ERR_NO_MEM;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_ERR_NO_MEM && live_bus && !live_device);
    assert(ryz_i2c_scan_bus_resources_held());
    add_error = ESP_OK;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK);
    assert(ryz_i2c_scan_bus_end() == ESP_OK && !ryz_i2c_scan_bus_resources_held());
    assert(bus_deletes == 2 && device_deletes == 1);
}
static void cleanup(void)
{
    const ryz_i2c_scan_config_t config = {13,14,100000};
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK);
    remove_error = ESP_ERR_INVALID_STATE;
    assert(ryz_i2c_scan_bus_end() == remove_error && live_device && live_bus);
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_INVALID_STATE);
    assert(ryz_i2c_scan_bus_begin(&config) == remove_error && bus_creates == 1);
    remove_error = ESP_OK; delete_error = ESP_ERR_INVALID_STATE;
    assert(ryz_i2c_scan_bus_end() == delete_error && !live_device && live_bus);
    const unsigned old_removes = device_deletes;
    assert(ryz_i2c_scan_bus_end() == delete_error && device_deletes == old_removes);
    delete_error = ESP_OK; reset_error = ESP_FAIL;
    assert(ryz_i2c_scan_bus_end() == reset_error && !live_bus && !live_device);
    const unsigned old_deletes = bus_deletes;
    assert(ryz_tool_pins_check_pair(13,14) == ESP_ERR_INVALID_STATE);
    assert(ryz_i2c_scan_bus_end() == reset_error && bus_deletes == old_deletes);
    reset_error = ESP_OK;
    assert(ryz_i2c_scan_bus_end() == ESP_OK && !ryz_i2c_scan_bus_resources_held());
    assert(ryz_tool_pins_check_pair(13,14) == ESP_OK);
}
static void admission(void)
{
    const ryz_i2c_scan_config_t config = {13,14,100000};
    uint32_t other = 0;
    assert(ryz_tool_pins_claim_pair(13,14,&other) == ESP_OK);
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_ERR_INVALID_STATE && !bus_creates);
    assert(ryz_tool_pins_release(other) == ESP_OK);
    native_reserved = true;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_ERR_INVALID_STATE && !bus_creates && !resets);
    assert(!ryz_i2c_scan_bus_resources_held() && ryz_tool_pins_check_pair(13,14) == ESP_OK);
    native_reserved = false;
    affinity = -1;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_ERR_INVALID_STATE && !bus_creates);
    affinity = 1; core = 0;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_ERR_INVALID_STATE && !bus_creates);
    core = 1;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK);
    affinity = -1;
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_INVALID_STATE && !transfers);
    assert(ryz_i2c_scan_bus_end() == ESP_ERR_INVALID_STATE && !bus_deletes && !device_deletes);
    assert(ryz_i2c_scan_bus_resources_held());
    affinity = 1;
    assert(ryz_i2c_scan_bus_end() == ESP_OK);
}
static void second_restore(void)
{
    const ryz_i2c_scan_config_t config = {13,14,100000};
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK);
    reset_error = ESP_FAIL; reset_fail_at = 2;
    assert(ryz_i2c_scan_bus_end() == ESP_FAIL && resets == 2);
    assert(!live_bus && !live_device && ryz_i2c_scan_bus_resources_held());
    assert(ryz_tool_pins_check_pair(13,14) == ESP_ERR_INVALID_STATE);
    assert(ryz_i2c_scan_bus_end() == ESP_OK && resets == 4);
    assert(bus_deletes == 1 && device_deletes == 1);
    assert(!ryz_i2c_scan_bus_resources_held() && ryz_tool_pins_check_pair(13,14) == ESP_OK);
}
static void occupied_controller(void)
{
    const ryz_i2c_scan_config_t config = {13,14,100000};
    native_controller_occupied = true; /* different pins; pin check alone passes */
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_ERR_INVALID_STATE);
    assert(other_owner_pins_intact && bus_creates == 0 && resets == 0);
    assert(controller_checks == 1 && !ryz_i2c_scan_bus_resources_held());
    assert(ryz_tool_pins_check_pair(13,14) == ESP_OK);
    assert(ryz_i2c_scan_bus_end() == ESP_OK && !bus_deletes && !resets);
    native_controller_occupied = false;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK);
    assert(ryz_i2c_scan_bus_end() == ESP_OK && other_owner_pins_intact);
}
static void defaults(void)
{
    const ryz_i2c_scan_config_t config = RYZ_I2C_SCAN_DEFAULT_CONFIG;
    assert(ryz_i2c_scan_bus_begin(&config) == ESP_OK && board_inits == 1);
    assert(!ryz_i2c_scan_bus_resources_held());
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_OK && board_probes == 1);
    assert(ryz_i2c_scan_bus_end() == ESP_OK);
    assert(!bus_creates && !device_creates && !resets && !bus_deletes);
    assert(!controller_checks);
    assert(ryz_i2c_scan_bus_probe(0x18) == ESP_ERR_INVALID_STATE);
    const ryz_i2c_scan_config_t bad[] = {{41,40,400000},{40,41,100000},{13,13,100000},
        {13,14,50000},{19,20,100000},{45,13,100000},{-1,13,100000}};
    assert(ryz_i2c_scan_validate_config(NULL) == ESP_ERR_INVALID_ARG);
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        assert(ryz_i2c_scan_validate_config(&bad[i]) == ESP_ERR_INVALID_ARG);
        assert(ryz_i2c_scan_bus_begin(&bad[i]) == ESP_ERR_INVALID_ARG);
    }
    assert(!bus_creates && !resets);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "lifecycle")) lifecycle();
    else if (!strcmp(argv[1], "errors")) errors();
    else if (!strcmp(argv[1], "allocation")) allocation();
    else if (!strcmp(argv[1], "cleanup")) cleanup();
    else if (!strcmp(argv[1], "admission")) admission();
    else if (!strcmp(argv[1], "defaults")) defaults();
    else if (!strcmp(argv[1], "second_restore")) second_restore();
    else if (!strcmp(argv[1], "occupied_controller")) occupied_controller();
    else assert(false);
    puts("I2C_SCAN_BUS_PASS");
}
