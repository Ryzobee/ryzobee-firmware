/* Exercise real core + real ESP Adapter through the public start/snapshot
 * Interface. Only FreeRTOS allocations/task creation and board calls differ.
 * Task creation records the entry; this allocation test does not schedule it. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ryz_i2c_scan.h"
#include "ryz_i2c_scan_bus.h"
#include "ryz_tool_pins.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct fake_semaphore { bool live, mutex, token; };
static struct fake_semaphore objects[16];
static unsigned allocations, fail_allocation, live, deletes, task_calls;
static bool task_failure;

static SemaphoreHandle_t allocate(bool mutex)
{
    ++allocations;
    if (allocations == fail_allocation) return NULL;
    assert(allocations < 16);
    SemaphoreHandle_t object = &objects[allocations];
    *object = (struct fake_semaphore){.live = true, .mutex = mutex, .token = mutex};
    ++live;
    return object;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return allocate(true); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return allocate(false); }
void vSemaphoreDelete(SemaphoreHandle_t object)
{
    assert(object && object->live);
    object->live = false;
    --live;
    ++deletes;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t object, TickType_t timeout)
{
    assert(object && object->live && timeout == portMAX_DELAY);
    assert(object->mutex && object->token); /* Worker is not scheduled here. */
    object->token = false;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t object)
{
    assert(object && object->live);
    bool prior = object->token;
    object->token = true;
    return prior ? pdFALSE : pdTRUE;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name, uint32_t stack,
                       void *argument, UBaseType_t priority, TaskHandle_t *out, BaseType_t core)
{
    assert(entry && !strcmp(name, "ryz-i2c-scan"));
    assert(stack == 4096 && priority == 2 && !argument && !out && core == 1);
    ++task_calls;
    return task_failure ? pdFALSE : pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; abort(); }
int64_t esp_timer_get_time(void) { return 123000; }
esp_err_t ryz_i2c_scan_validate_config(const ryz_i2c_scan_config_t *config)
{ return config ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t ryz_tool_pins_check_pair(int first, int second)
{ (void)first; (void)second; return ESP_OK; }
esp_err_t ryz_i2c_scan_bus_begin(const ryz_i2c_scan_config_t *config) { (void)config; abort(); }
esp_err_t ryz_i2c_scan_bus_probe(uint8_t address) { (void)address; abort(); }
esp_err_t ryz_i2c_scan_bus_end(void) { abort(); }
bool ryz_i2c_scan_bus_resources_held(void) { abort(); }

int main(void)
{
    uint32_t id = 999;
    fail_allocation = allocations + 1;
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_NO_MEM && !id);
    assert(live == 0 && deletes == 0 && task_calls == 0);
    fail_allocation = allocations + 2;
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_NO_MEM && !id);
    assert(live == 0 && deletes == 1 && task_calls == 0);
    fail_allocation = 0;
    task_failure = true;
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_NO_MEM && !id);
    assert(live == 0 && deletes == 3 && task_calls == 1);
    ryz_i2c_scan_snapshot_t snapshot;
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(snapshot.scan_id == 0 && snapshot.phase == RYZ_I2C_SCAN_IDLE);
    task_failure = false;
    assert(ryz_i2c_scan_start(&id) == ESP_OK && id == 1);
    assert(live == 2 && deletes == 3 && task_calls == 2);
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_QUEUED && snapshot.queued_ms == 123);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK);
    assert(ryz_i2c_scan_cancel(id) == ESP_OK); /* Binary wake coalescing is harmless. */
    assert(ryz_i2c_scan_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_I2C_SCAN_CANCELLING && !snapshot.probe_calls);
    assert(ryz_i2c_scan_start(&id) == ESP_ERR_INVALID_STATE && id == 0);
    assert(task_calls == 2 && live == 2);
    puts("I2C_SCAN_PASS platform_alloc");
    return 0;
}
