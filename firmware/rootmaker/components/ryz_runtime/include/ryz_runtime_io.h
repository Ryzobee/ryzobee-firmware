#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ryz_ui.h"
#include "touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RYZ_IO_CLEAR,
    RYZ_IO_READ_PIXEL,
    RYZ_IO_RECT,
    RYZ_IO_TEXT,
    RYZ_IO_SHOW,
    RYZ_IO_NEURO_PAINT,
    RYZ_IO_ABORT,
    RYZ_IO_TOUCH_INFO,
    RYZ_IO_TOUCH_READ,
    RYZ_IO_TOUCH_DEMO,
    RYZ_IO_UI_MOUNT,
    RYZ_IO_UI_PUMP,
    RYZ_IO_UI_CLOSE,
    RYZ_IO_UI_UPDATE,
} ryz_runtime_io_kind_t;

/* Trusted C-only, synchronous request. The transport borrows this request,
 * text, scene and cancellation context until execution or cancellation has
 * been acknowledged; a caller timeout must not release any of them early.
 * Neither the cancellation callback nor the input Adapter may call Lua or
 * re-enter the display/LVGL owner. They run on that owner, not the Lua worker.
 * Output fields are status, color (READ_PIXEL), done (NEURO_PAINT), touch,
 * touch_info and demo_enabled. Requests must not be serialized to user Lua. */
typedef struct ryz_runtime_io_request {
    ryz_runtime_io_kind_t kind;
    esp_err_t status;
    int x, y, width, height, scale;
    uint16_t color;
    const char *text;
    size_t text_length;
    bool enabled, begin, done;
    const ryz_ui_scene_t *scene;
    ryz_touch_sample_t touch;
    ryz_touch_info_t touch_info;
    bool demo_enabled;
    bool (*cancelled)(void *context);
    void *cancel_context;
} ryz_runtime_io_request_t;

/* Execute only on the single LCD/TP/LVGL owner (or before its startup, while
 * boot remains single-threaded). No locking or queuing occurs here. A null
 * read_touch uses the board directly for bootstrap; a running worker should
 * receive input through the owner's input Adapter. Cleanup ignores cancel.
 * NEURO_PAINT may retain in-flight DMA: no drawing until done or ABORT.
 * Cancellation cannot interrupt a DMA drain or guarantee a hardware-fault
 * time bound. All borrowed data remains live until this function returns. */
void ryz_runtime_io_execute(
    ryz_runtime_io_request_t *request,
    esp_err_t (*read_touch)(void *context, ryz_touch_sample_t *sample),
    void *input_context);

bool ryz_runtime_io_is_cleanup(const ryz_runtime_io_request_t *request);

#ifdef __cplusplus
}
#endif
