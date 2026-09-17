#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ryz_pixels_host.h"

struct ryz_host_mutex { pthread_mutex_t mutex; };
typedef union {
    max_align_t alignment;
    struct { size_t bytes; uint32_t marker; } value;
} allocation_t;
static size_t s_blocks, s_bytes, s_peak, s_fail_after = SIZE_MAX;
static bool s_mutex_fail;
static _Thread_local char s_task_identity;

void *heap_caps_calloc(size_t count, size_t bytes, uint32_t capabilities)
{
    assert(capabilities == (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (count && bytes > (SIZE_MAX - sizeof(allocation_t)) / count) return NULL;
    if (s_fail_after == 0) return NULL;
    if (s_fail_after != SIZE_MAX) --s_fail_after;
    size_t size = count * bytes;
    allocation_t *allocation = calloc(1, sizeof(*allocation) + size);
    if (!allocation) return NULL;
    allocation->value.bytes = size;
    allocation->value.marker = 0x52595a46;
    ++s_blocks;
    s_bytes += size;
    if (s_bytes > s_peak) s_peak = s_bytes;
    return allocation + 1;
}

void *heap_caps_malloc(size_t bytes, uint32_t capabilities)
{
    return heap_caps_calloc(1, bytes, capabilities);
}

void heap_caps_free(void *pointer)
{
    if (!pointer) return;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    assert(allocation->value.marker == 0x52595a46 && s_blocks > 0);
    --s_blocks;
    s_bytes -= allocation->value.bytes;
    allocation->value.marker = 0;
    free(allocation);
}
void ryz_host_heap_fail_after(size_t count) { s_fail_after = count; }
void ryz_host_heap_allow(void) { s_fail_after = SIZE_MAX; }
size_t ryz_host_heap_blocks(void) { return s_blocks; }
size_t ryz_host_heap_bytes(void) { return s_bytes; }
size_t ryz_host_heap_peak(void) { return s_peak; }
void ryz_host_mutex_fail(bool fail) { s_mutex_fail = fail; }

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t mutex = malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) { free(mutex); return NULL; }
    return mutex;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    assert(timeout == portMAX_DELAY);
    if (s_mutex_fail) return 0;
    return pthread_mutex_lock(&mutex->mutex) == 0 ? pdTRUE : 0;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    return pthread_mutex_unlock(&mutex->mutex) == 0 ? pdTRUE : 0;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_destroy(&mutex->mutex) == 0);
    free(mutex);
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_task_identity; }
void ryz_host_yield(void) { sched_yield(); }

const char *esp_err_to_name(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_HOST_ERROR";
    }
}
