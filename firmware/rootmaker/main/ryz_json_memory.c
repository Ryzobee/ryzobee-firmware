#include "ryz_json_memory.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <stdbool.h>

static void *json_allocate(size_t size)
{
    /* JSON nodes, strings and wire text are CPU-only. Do not consume the
     * internal DMA reserve needed by simultaneous Wi-Fi and BLE operation. */
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void ryz_json_memory_init(void)
{
    /* app_main is the sole caller, before the first application task. This
     * guard is not a runtime lock: installing hooks concurrently is forbidden.
     * ESP heap_caps_free/free both accept internal and external allocations. */
    static bool initialized;
    if (initialized) return;
    cJSON_Hooks hooks = { .malloc_fn = json_allocate, .free_fn = heap_caps_free };
    cJSON_InitHooks(&hooks);
    initialized = true;
}
