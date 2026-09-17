#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i2c_private.h"

esp_err_t __wrap_i2c_acquire_bus_handle(i2c_port_num_t port_num,
                                      i2c_bus_handle_t *out, i2c_bus_mode_t mode);

static esp_err_t sdk_error;
static i2c_bus_handle_t sdk_handle;
static bool sdk_writes_out = true;
static i2c_port_num_t expected_port;
static i2c_bus_mode_t expected_mode;
static i2c_bus_handle_t *expected_out;
static unsigned sdk_calls, dereferences;
static struct i2c_bus_t valid_bus = {.port_num = 1};

/* Only the SDK allocator/acquire return is injected. The production wrapper
 * calls this once; it must not change arguments, handles, or error semantics. */
esp_err_t __real_i2c_acquire_bus_handle(i2c_port_num_t port_num,
                                      i2c_bus_handle_t *out, i2c_bus_mode_t mode)
{
    ++sdk_calls;
    assert(port_num == expected_port && mode == expected_mode && out == expected_out);
    if (sdk_writes_out) {
        assert(out);
        *out = sdk_handle;
    }
    return sdk_error;
}

/* Reproduce the audited master + slave call pattern: an acquire error goes
 * to cleanup; success immediately dereferences the base. This is a simulated
 * caller, not the SDK's hardware constructor or actual heap allocator.
 * macOS has no GNU --wrap; the target link is verified separately. */
static esp_err_t simulated_sdk_constructor(i2c_port_num_t port, i2c_bus_mode_t mode)
{
    i2c_bus_handle_t base = NULL;
    expected_out = &base;
    expected_port = port;
    expected_mode = mode;
    esp_err_t error = __wrap_i2c_acquire_bus_handle(port, &base, mode);
    if (error != ESP_OK) return error;
    ++dereferences;
    int resolved_port = base->port_num;
    assert(resolved_port == 1 && base == &valid_bus);
    return ESP_OK;
}

static void null_success(i2c_bus_mode_t mode)
{
    sdk_error = ESP_OK;
    sdk_handle = NULL;
    assert(simulated_sdk_constructor(1, mode) == ESP_ERR_NO_MEM);
    assert(sdk_calls == 1 && dereferences == 0);
}

static void success(void)
{
    sdk_handle = &valid_bus;
    sdk_error = ESP_OK;
    for (int port = -1; port <= 1; ++port) {
        assert(simulated_sdk_constructor(port, I2C_BUS_MODE_MASTER) == ESP_OK);
        assert(simulated_sdk_constructor(port, I2C_BUS_MODE_SLAVE) == ESP_OK);
    }
    assert(sdk_calls == 6 && dereferences == 6 && valid_bus.port_num == 1);
}

static void errors(void)
{
    const esp_err_t errors[] = {ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE, ESP_FAIL};
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
            for (unsigned has_handle = 0; has_handle < 2; ++has_handle) {
                sdk_error = errors[i];
                sdk_handle = has_handle ? &valid_bus : NULL;
                i2c_bus_handle_t out = &valid_bus;
                expected_out = &out;
                expected_port = -1;
                expected_mode = (i2c_bus_mode_t)mode;
                assert(__wrap_i2c_acquire_bus_handle(-1, &out, expected_mode) == errors[i]);
                assert(out == sdk_handle); /* Do not steal/release a duplicate SDK base. */
            }
        }
    }
    sdk_writes_out = false;
    sdk_error = ESP_ERR_INVALID_ARG;
    expected_out = NULL;
    expected_port = 0;
    expected_mode = I2C_BUS_MODE_MASTER;
    assert(__wrap_i2c_acquire_bus_handle(0, NULL, I2C_BUS_MODE_MASTER) == ESP_ERR_INVALID_ARG);
    assert(sdk_calls == 17 && !dereferences && valid_bus.port_num == 1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "master_null_success")) null_success(I2C_BUS_MODE_MASTER);
    else if (!strcmp(argv[1], "slave_null_success")) null_success(I2C_BUS_MODE_SLAVE);
    else if (!strcmp(argv[1], "success")) success();
    else if (!strcmp(argv[1], "errors")) errors();
    else abort();
    printf("I2C_IDF_COMPAT_PASS %s\n", argv[1]);
    return 0;
}
