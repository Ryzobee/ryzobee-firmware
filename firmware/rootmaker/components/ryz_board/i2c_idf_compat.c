#include <stddef.h>
#include "esp_idf_version.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4)
#error "Re-audit the I2C base-allocation compatibility wrapper for this ESP-IDF version"
#endif

#include "i2c_private.h"

esp_err_t __real_i2c_acquire_bus_handle(i2c_port_num_t port_num,
                                      i2c_bus_handle_t *out, i2c_bus_mode_t mode);

esp_err_t __wrap_i2c_acquire_bus_handle(i2c_port_num_t port_num,
                                      i2c_bus_handle_t *out, i2c_bus_mode_t mode)
{
    /* IDF 5.5.4 i2c_common.c leaves ret=ESP_OK when the base calloc fails.
     * Both master/slave constructors dereference that base after success.
     * Correct only the impossible success; SDK retains all resource ownership,
     * including handles returned alongside an existing non-OK error. */
    esp_err_t error = __real_i2c_acquire_bus_handle(port_num, out, mode);
    return error == ESP_OK && out && *out == NULL ? ESP_ERR_NO_MEM : error;
}
