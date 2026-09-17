#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RYZ_TOUCH_WIDTH 240
#define RYZ_TOUCH_HEIGHT 240

typedef enum {
    RYZ_TOUCH_NONE, RYZ_TOUCH_DOWN, RYZ_TOUCH_MOVE, RYZ_TOUCH_UP
} ryz_touch_event_t;

typedef struct {
    bool pressed;
    /* Set only by an input broker when a previously delivered gesture became
     * unknowable. Physical decoders always leave this false. */
    bool interrupted;
    bool has_position;
    uint16_t x;
    uint16_t y;
    uint8_t fingers;
    uint8_t gesture;
    uint8_t raw_event;
    ryz_touch_event_t event;
    uint32_t sampled_ms;
} ryz_touch_sample_t;

/* CST816T/D registers 0x01..0x06, one point. No I/O or retained state.
 * Up coordinates are unreliable and ignored. The BSP's inclusive edge 240
 * maps to pixel 239; other out-of-range coordinates are rejected. */
bool ryz_touch_decode(const uint8_t data[6], ryz_touch_sample_t *sample);
/* Normalize controller-reported phases into transitions between successful
 * samples. raw_event remains untouched for controller diagnostics. */
void ryz_touch_normalize_event(bool previous_valid, bool previous_pressed,
                               ryz_touch_sample_t *sample);
/* Inverse of display's clockwise quarter-turns. Positionless samples retain
 * their event/identity; invalid rotation or coordinates are rejected. */
bool ryz_touch_rotate(ryz_touch_sample_t *sample, uint8_t rotation);
const char *ryz_touch_event_name(ryz_touch_event_t event);
/* Only explicitly verified variants are accepted; NULL means unsupported. */
const char *ryz_touch_controller_name(uint8_t chip_id);
