/* Exercise production owner/probes/getter. Reuse the existing external
 * NVS/HCI/NPL/task fake, replacing only its all-success port initialization.
 * The host invokes the production __wrap entry points explicitly: GNU target
 * linker rewriting is a separate ELF/real-device verification, not proven by
 * this test. No failure here is claimed to reproduce RF or hardware OOM. */
#define main existing_backend_test_main
#define nimble_port_init uninstrumented_nimble_port_init
#include "ble_backend_test.c"
#undef nimble_port_init
#undef main

#include "esp_bt.h"
#include "esp_heap_caps.h"
#include <stdarg.h>

enum { INIT_OK, BUFFER_FAILURE, VHCI_FAILURE, LATER_PORT_FAILURE };
static unsigned stage, buffer_calls, vhci_calls, logs;
static esp_vhci_host_callback_t callback;

esp_err_t __wrap_ble_buf_alloc(void);
esp_err_t __wrap_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *);

size_t heap_caps_get_free_size(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ryz_ble_init_diagnostics_t d;
    /* Acquiring the same production guard proves it is not held over SDK
     * heap calls; a lock regression is caught by the runner's timeout. */
    assert(ryz_ble_get_init_diagnostics(&d) == ESP_OK);
    return 50000 - buffer_calls * 1000 - vhci_calls * 100;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return 20000 - buffer_calls * 500;
}

void diag_test_log(const char *format, ...)
{
    assert(strstr(format, "startup failed"));
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(strstr(line, stage == BUFFER_FAILURE ? "stage=buffers" :
                       stage == VHCI_FAILURE ? "stage=vhci" : "stage=port"));
    assert(!strstr(line, "DEBUG-") && !strstr(line, "address") && !strstr(line, "key"));
    ++logs;
}

esp_err_t __real_ble_buf_alloc(void)
{
    ryz_ble_init_diagnostics_t d;
    assert(ryz_ble_get_init_diagnostics(&d) == ESP_OK);
    if (!d.finished) assert(d.buffers.entered && !d.buffers.returned);
    ++buffer_calls;
    return stage == BUFFER_FAILURE ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t __real_esp_vhci_host_register_callback(const esp_vhci_host_callback_t *cb)
{
    assert(cb == &callback);
    ryz_ble_init_diagnostics_t d;
    assert(ryz_ble_get_init_diagnostics(&d) == ESP_OK);
    if (!d.finished) assert(d.buffers.returned && d.vhci.entered && !d.vhci.returned);
    ++vhci_calls;
    return stage == VHCI_FAILURE ? ESP_FAIL : ESP_OK;
}

esp_err_t nimble_port_init(void)
{
    assert(logs == 0);
    if (__wrap_ble_buf_alloc() != ESP_OK) return ESP_FAIL;
    assert(logs == 0);
    if (__wrap_esp_vhci_host_register_callback(&callback) != ESP_OK) return ESP_FAIL;
    assert(logs == 0);
    return stage == LATER_PORT_FAILURE ? ESP_FAIL : ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    stage = (unsigned)atoi(argv[1]);
    assert(stage <= LATER_PORT_FAILURE);
    ryz_ble_init_diagnostics_t d;
    assert(ryz_ble_get_init_diagnostics(NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_ble_get_init_diagnostics(&d) == ESP_OK);
    assert(!d.finished && !d.port.entered && !d.buffers.entered && !d.vhci.entered);

    start();
    assert(ryz_ble_get_init_diagnostics(&d) == ESP_OK);
    assert(d.finished && d.port.entered && d.port.returned);
    assert(d.port.result == (stage ? ESP_FAIL : ESP_OK));
    assert(d.buffers.entered && d.buffers.returned && buffer_calls == 1);
    assert(d.buffers.result == (stage == BUFFER_FAILURE ? ESP_ERR_NO_MEM : ESP_OK));
    assert(d.port.internal_free_before == 50000 && d.port.internal_largest_before == 20000);
    assert(d.buffers.internal_free_after == 49000 && d.buffers.internal_largest_after == 19500);
    assert(d.vhci.entered == (stage != BUFFER_FAILURE));
    assert(d.vhci.returned == (stage != BUFFER_FAILURE));
    assert(vhci_calls == (stage != BUFFER_FAILURE));
    if (stage != BUFFER_FAILURE) {
        assert(d.vhci.result == (stage == VHCI_FAILURE ? ESP_FAIL : ESP_OK));
        assert(d.vhci.internal_free_before == 49000 && d.vhci.internal_free_after == 48900);
    }
    assert(logs == (stage != INIT_OK) && snapshot().available == (stage == INIT_OK));

    /* Later cleanup remains transparent but cannot replace the first startup
     * diagnostics. A zero value in an unentered VHCI record is not success. */
    assert(__wrap_ble_buf_alloc() == (stage == BUFFER_FAILURE ? ESP_ERR_NO_MEM : ESP_OK));
    assert(__wrap_esp_vhci_host_register_callback(&callback) ==
           (stage == VHCI_FAILURE ? ESP_FAIL : ESP_OK));
    assert(buffer_calls == 2 && vhci_calls == 1U + (stage != BUFFER_FAILURE));
    ryz_ble_init_diagnostics_t after;
    assert(ryz_ble_get_init_diagnostics(&after) == ESP_OK);
    assert(!memcmp(&after, &d, sizeof(d)) && logs == (stage != INIT_OK));
    printf("BLE_INIT_DIAGNOSTICS_PASS stage=%u\n", stage);
    return 0;
}
