#include "touch_protocol.h"
#include <string.h>

const char *ryz_touch_controller_name(uint8_t chip_id)
{
    switch (chip_id) {
    case 0xb5: return "CST816T";
    case 0xb6: return "CST816D";
    default: return NULL;
    }
}

bool ryz_touch_decode(const uint8_t data[6], ryz_touch_sample_t *sample)
{
    if (!data || !sample) return false;
    memset(sample, 0, sizeof(*sample));
    sample->gesture = data[0];
    sample->fingers = data[1];
    sample->raw_event = data[2] >> 6;
    if (sample->fingers > 1) return false;
    if (sample->raw_event == 1) {
        sample->event = RYZ_TOUCH_UP;
        return true;
    }
    if (!sample->fingers) return true;
    if (sample->raw_event == 3) return false;
    uint16_t x = ((uint16_t)(data[2] & 0x0f) << 8) | data[3];
    uint16_t y = ((uint16_t)(data[4] & 0x0f) << 8) | data[5];
    if (x > RYZ_TOUCH_WIDTH || y > RYZ_TOUCH_HEIGHT) return false;
    sample->x = x == RYZ_TOUCH_WIDTH ? x - 1 : x;
    sample->y = y == RYZ_TOUCH_HEIGHT ? y - 1 : y;
    sample->pressed = true;
    sample->has_position = true;
    sample->event = sample->raw_event == 0 ? RYZ_TOUCH_DOWN : RYZ_TOUCH_MOVE;
    return true;
}

void ryz_touch_normalize_event(bool previous_valid, bool previous_pressed,
                               ryz_touch_sample_t *sample)
{
    if (!sample) return;
    if (sample->pressed) {
        sample->event = (!previous_valid || !previous_pressed) ? RYZ_TOUCH_DOWN : RYZ_TOUCH_MOVE;
    } else {
        sample->event = previous_valid && previous_pressed ? RYZ_TOUCH_UP : RYZ_TOUCH_NONE;
    }
}

const char *ryz_touch_event_name(ryz_touch_event_t event)
{
    switch (event) {
    case RYZ_TOUCH_DOWN: return "down";
    case RYZ_TOUCH_MOVE: return "move";
    case RYZ_TOUCH_UP: return "up";
    default: return "none";
    }
}

bool ryz_touch_rotate(ryz_touch_sample_t *sample, uint8_t rotation)
{
    if (!sample || rotation > 3) return false;
    if (!sample->has_position) return true;
    if (sample->x >= RYZ_TOUCH_WIDTH || sample->y >= RYZ_TOUCH_HEIGHT) return false;
    uint16_t x = sample->x, y = sample->y;
    switch (rotation) {
    case 1: sample->x = y; sample->y = RYZ_TOUCH_WIDTH - 1U - x; break;
    case 2: sample->x = RYZ_TOUCH_WIDTH - 1U - x; sample->y = RYZ_TOUCH_HEIGHT - 1U - y; break;
    case 3: sample->x = RYZ_TOUCH_HEIGHT - 1U - y; sample->y = x; break;
    default: break;
    }
    return true;
}
