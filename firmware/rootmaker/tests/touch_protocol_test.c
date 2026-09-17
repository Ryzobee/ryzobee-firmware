#include "touch_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(!strcmp(ryz_touch_controller_name(0xb5), "CST816T"));
    assert(!strcmp(ryz_touch_controller_name(0xb6), "CST816D"));
    for (unsigned id = 0; id <= 255; ++id) {
        assert((ryz_touch_controller_name((uint8_t)id) != NULL) == (id == 0xb5 || id == 0xb6));
    }
    ryz_touch_sample_t s;
    for(unsigned x=0;x<240;++x) for(unsigned y=0;y<240;++y) {
        for(unsigned r=0;r<4;++r) {
            s=(ryz_touch_sample_t){.pressed=true,.has_position=true,.x=x,.y=y};
            assert(ryz_touch_rotate(&s,r));
            assert(s.x<240 && s.y<240);
            assert(ryz_touch_rotate(&s,(4-r)%4));
            assert(s.x==x && s.y==y);
        }
    }
    s=(ryz_touch_sample_t){.pressed=true,.has_position=true,.x=0,.y=239};
    assert(ryz_touch_rotate(&s,1) && s.x==239 && s.y==239);
    assert(!ryz_touch_rotate(&s,4) && !ryz_touch_rotate(NULL,0));
    s.x=240;
    assert(!ryz_touch_rotate(&s,0));
    s=(ryz_touch_sample_t){0};
    assert(ryz_touch_rotate(&s,3) && !s.has_position);
    uint8_t data[6] = {0};
    assert(ryz_touch_decode(data, &s) && !s.pressed && !s.has_position && s.event == RYZ_TOUCH_NONE);
    data[1] = 1;
    assert(ryz_touch_decode(data, &s) && s.pressed && s.x == 0 && s.y == 0 && s.event == RYZ_TOUCH_DOWN);
    data[2] = 0x80;
    data[3] = 120;
    data[5] = 239;
    assert(ryz_touch_decode(data, &s) && s.pressed && s.x == 120 && s.y == 239 && s.event == RYZ_TOUCH_MOVE);

    /* Every valid coordinate, inclusive BSP edges, and just-outside values. */
    unsigned pairs = 0;
    for (unsigned x = 0; x <= 241; ++x) {
        for (unsigned y = 0; y <= 241; ++y) {
            data[2] = (uint8_t)(0x80 | (x >> 8));
            data[3] = (uint8_t)x;
            data[4] = (uint8_t)(y >> 8);
            data[5] = (uint8_t)y;
            bool valid = ryz_touch_decode(data, &s);
            assert(valid == (x <= 240 && y <= 240));
            if (valid) {
                assert(s.has_position && s.pressed);
                assert(s.x == (x == 240 ? 239 : x) && s.y == (y == 240 ? 239 : y));
            } else assert(!s.pressed && !s.has_position);
            ++pairs;
        }
    }

    /* UP may still report one finger and garbage coordinates. Never plot it. */
    const uint8_t up[6] = {5, 1, 0x4f, 0xff, 0x0f, 0xff};
    assert(ryz_touch_decode(up, &s) && !s.pressed && !s.has_position && s.event == RYZ_TOUCH_UP);
    assert(s.gesture == 5 && s.raw_event == 1);
    const uint8_t idle_up[6] = {0, 0, 0x40, 17, 0, 20};
    assert(ryz_touch_decode(idle_up, &s) && !s.pressed && !s.has_position && s.event == RYZ_TOUCH_UP);
    const uint8_t multi[6] = {0, 2, 0, 10, 0, 10};
    assert(!ryz_touch_decode(multi, &s));
    const uint8_t reserved[6] = {0, 1, 0xc0, 10, 0, 10};
    assert(!ryz_touch_decode(reserved, &s));
    const uint8_t huge[6] = {0, 1, 0x8f, 0xff, 0x0f, 0xff};
    assert(!ryz_touch_decode(huge, &s));
    const uint8_t reserved_bits[6] = {0, 1, 0xb0, 10, 0xf0, 20};
    assert(ryz_touch_decode(reserved_bits, &s) && s.x == 10 && s.y == 20 && s.event == RYZ_TOUCH_MOVE);
    assert(!ryz_touch_decode(NULL, &s) && !ryz_touch_decode(data, NULL));

    /* A polled CST816D can report MOVE as the first observed pressed sample.
     * The public event stream follows sampled state transitions instead of
     * requiring the short-lived controller DOWN phase to be observed. */
    s = (ryz_touch_sample_t){
        .pressed = true, .has_position = true, .x = 155, .y = 138,
        .raw_event = 2, .event = RYZ_TOUCH_MOVE,
    };
    ryz_touch_normalize_event(true, false, &s);
    assert(s.event == RYZ_TOUCH_DOWN && s.raw_event == 2 && s.x == 155 && s.y == 138);
    ryz_touch_normalize_event(true, true, &s);
    assert(s.event == RYZ_TOUCH_MOVE && s.raw_event == 2);
    s = (ryz_touch_sample_t){.raw_event = 0, .event = RYZ_TOUCH_NONE};
    ryz_touch_normalize_event(true, true, &s);
    assert(s.event == RYZ_TOUCH_UP && !s.pressed && !s.has_position);
    s.event = RYZ_TOUCH_UP;
    ryz_touch_normalize_event(true, false, &s);
    assert(s.event == RYZ_TOUCH_NONE);

    assert(!strcmp(ryz_touch_event_name(RYZ_TOUCH_DOWN), "down"));
    assert(!strcmp(ryz_touch_event_name(RYZ_TOUCH_MOVE), "move"));
    assert(!strcmp(ryz_touch_event_name(RYZ_TOUCH_UP), "up"));
    assert(!strcmp(ryz_touch_event_name(RYZ_TOUCH_NONE), "none"));
    printf("TOUCH_PROTOCOL_PASS coordinate_pairs=%u plus state/error fixtures\n", pairs);
    return 0;
}
