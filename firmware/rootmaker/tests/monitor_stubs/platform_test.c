/* Real core + fixed stream + ESP OS Adapter. Task creation records/checks
 * configuration but does not schedule a task in this allocation test. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ryz_monitor.h"
#include "ryz_monitor_source.h"
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
    ++live; return object;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return allocate(true); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return allocate(false); }
void vSemaphoreDelete(SemaphoreHandle_t object)
{ assert(object && object->live); object->live = false; --live; ++deletes; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t object, TickType_t timeout)
{
    assert(object && object->live && timeout == portMAX_DELAY);
    assert(object->mutex && object->token); object->token = false; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t object)
{
    assert(object && object->live); bool prior = object->token;
    object->token = true; return prior ? pdFALSE : pdTRUE;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name, uint32_t stack,
                       void *argument, UBaseType_t priority, TaskHandle_t *out, BaseType_t core)
{
    assert(entry && !strcmp(name, "ryz-monitor"));
    assert(stack == 4096 && priority == 2 && !argument && !out && core == 1);
    ++task_calls; return task_failure ? pdFALSE : pdPASS;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; abort(); }
int64_t esp_timer_get_time(void) { return 123000; }
esp_err_t ryz_monitor_validate_config(const ryz_monitor_config_t *config)
{ return config ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t ryz_monitor_uart_begin(const ryz_monitor_config_t *config) { (void)config; abort(); }
esp_err_t ryz_monitor_uart_poll(ryz_monitor_sample_t *out) { (void)out; abort(); }
esp_err_t ryz_monitor_uart_end(void) { abort(); }
bool ryz_monitor_uart_resources_held(void) { abort(); }
esp_err_t ryz_monitor_log_install(void) { abort(); }
void ryz_monitor_log_capture(bool enabled) { (void)enabled; abort(); }
int main(void)
{
    uint32_t id = 999;
    ryz_monitor_config_t config = {RYZ_MONITOR_UART, 2, 3, 115200};
    fail_allocation = allocations + 1;
    assert(ryz_monitor_start(1, &id) == ESP_ERR_NO_MEM && !id);
    assert(!live && !deletes && !task_calls);
    fail_allocation = allocations + 2;
    assert(ryz_monitor_configure(&config, 1, 0, &id) == ESP_ERR_NO_MEM && !id);
    assert(!live && deletes == 1 && !task_calls);
    fail_allocation = 0; task_failure = true;
    assert(ryz_monitor_start(1, &id) == ESP_ERR_NO_MEM && !id);
    assert(!live && deletes == 3 && task_calls == 1);
    ryz_monitor_snapshot_t snapshot;
    memset(&snapshot, 0xff, sizeof(snapshot));
    assert(ryz_monitor_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(!snapshot.config_revision && !snapshot.session_id);
    task_failure = false;
    assert(ryz_monitor_configure(&config, 1, 0, &id) == ESP_OK && id == 1);
    assert(live == 2 && deletes == 3 && task_calls == 2);
    assert(ryz_monitor_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_MONITOR_RECONFIGURING && snapshot.operation_pending);
    assert(snapshot.config_revision == 1 && snapshot.config.source == RYZ_MONITOR_SYSTEM);
    assert(snapshot.requested_config.source == RYZ_MONITOR_UART && !snapshot.source_active);
    assert(ryz_monitor_start(1, &id) == ESP_ERR_INVALID_STATE && !id);
    assert(task_calls == 2 && live == 2);
    puts("MONITOR_PASS platform_alloc");
    return 0;
}
