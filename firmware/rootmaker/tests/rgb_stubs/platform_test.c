/* Real core + ESP Adapter, with only FreeRTOS and the external driver replaced.
 * Task creation checks configuration without scheduling the recorded entry. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ryz_rgb.h"
#include "ryz_rgb_driver.h"
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
    assert(entry && !strcmp(name, "ryz-rgb"));
    assert(stack == 4096 && priority == 2 && !argument && !out && core == 1);
    ++task_calls;
    return task_failure ? pdFALSE : pdPASS;
}
int64_t esp_timer_get_time(void) { return 123000; }
esp_err_t ryz_rgb_driver_write(ryz_rgb_color_t color) { (void)color; abort(); }
esp_err_t ryz_rgb_driver_cleanup(void) { abort(); }
ryz_rgb_driver_status_t ryz_rgb_driver_get_status(void) { abort(); }

int main(void)
{
    ryz_rgb_color_t black = {0};
    uint32_t id = 999;
    fail_allocation = allocations + 1;
    assert(ryz_rgb_submit(black, &id) == ESP_ERR_NO_MEM && !id);
    assert(live == 0 && deletes == 0 && task_calls == 0);
    fail_allocation = allocations + 2;
    assert(ryz_rgb_submit(black, &id) == ESP_ERR_NO_MEM && !id);
    assert(live == 0 && deletes == 1 && task_calls == 0);
    fail_allocation = 0;
    task_failure = true;
    assert(ryz_rgb_submit(black, &id) == ESP_ERR_NO_MEM && !id);
    assert(live == 0 && deletes == 3 && task_calls == 1);
    ryz_rgb_snapshot_t snapshot;
    memset(&snapshot, 0xff, sizeof(snapshot));
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(snapshot.request_id == 0 && snapshot.phase == RYZ_RGB_IDLE);
    assert(!snapshot.output_known && !snapshot.last_completed_id);
    task_failure = false;
    assert(ryz_rgb_submit(black, &id) == ESP_OK && id == 1);
    assert(live == 2 && deletes == 3 && task_calls == 2);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.phase == RYZ_RGB_QUEUED && snapshot.queued_ms == 123);
    assert(!snapshot.requested.red && !snapshot.requested.green && !snapshot.requested.blue);
    assert(!snapshot.output_known && !snapshot.last_completed_id && !snapshot.finished_ms);
    assert(ryz_rgb_submit((ryz_rgb_color_t){255, 255, 255}, &id) == ESP_ERR_INVALID_STATE);
    assert(!id && task_calls == 2 && live == 2);
    assert(ryz_rgb_get_snapshot(&snapshot) == ESP_OK && snapshot.request_id == 1);
    assert(!snapshot.requested.red && !snapshot.requested.green && !snapshot.requested.blue);
    puts("RGB_PASS platform_alloc");
    return 0;
}
