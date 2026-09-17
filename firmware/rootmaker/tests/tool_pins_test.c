#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "ryz_tool_pins.h"

static void pairs(void)
{
    uint32_t first = 0, second = 0;
    assert(ryz_tool_pins_claim_pair(13, 14, &first) == ESP_OK && first);
    assert(ryz_tool_pins_check_pair(14, 15) == ESP_ERR_INVALID_STATE);
    assert(ryz_tool_pins_claim_pair(14, 15, &second) == ESP_ERR_INVALID_STATE && !second);
    assert(ryz_tool_pins_check_pair(15, 16) == ESP_OK);
    assert(ryz_tool_pins_release(first) == ESP_OK);
    assert(ryz_tool_pins_claim_pair(14, 13, &second) == ESP_OK && second > first);
    assert(ryz_tool_pins_release(first) == ESP_ERR_INVALID_STATE);
    assert(ryz_tool_pins_check_pair(13, 15) == ESP_ERR_INVALID_STATE);
    assert(ryz_tool_pins_release(second) == ESP_OK);
}

static void eligibility(void)
{
    const int always[] = {13,14,15,16,17,18,21,38,47,48};
    const int board[] = {0,1,2,3,5,7,9,10,12,40,41,42,43,44,45,46};
    const int hidden[] = {4,6,8,11};
    for (unsigned i = 0; i < sizeof(always)/sizeof(always[0]); ++i) {
        assert(ryz_tool_pin_reason(always[i]) == RYZ_TOOL_PIN_AVAILABLE);
    }
    for (unsigned i = 0; i < sizeof(board)/sizeof(board[0]); ++i) {
        assert(ryz_tool_pin_reason(board[i]) == RYZ_TOOL_PIN_BOARD);
    }
    for (unsigned i = 0; i < sizeof(hidden)/sizeof(hidden[0]); ++i) {
        assert(ryz_tool_pin_reason(hidden[i]) == RYZ_TOOL_PIN_NOT_EXPOSED);
    }
    for (int pin = 26; pin <= 32; ++pin) assert(ryz_tool_pin_reason(pin) == RYZ_TOOL_PIN_MEMORY);
    for (int pin = 33; pin <= 37; ++pin) {
#if CONFIG_ESPTOOLPY_OCT_FLASH || CONFIG_SPIRAM_MODE_OCT
        assert(ryz_tool_pin_reason(pin) == RYZ_TOOL_PIN_MEMORY);
        assert(ryz_tool_pins_check_pair(pin,13) == ESP_ERR_INVALID_ARG);
#else
        assert(ryz_tool_pin_reason(pin) == RYZ_TOOL_PIN_AVAILABLE);
        assert(ryz_tool_pins_check_pair(pin,13) == ESP_OK);
#endif
    }
    assert(ryz_tool_pin_reason(19) == RYZ_TOOL_PIN_USB && ryz_tool_pin_reason(20) == RYZ_TOOL_PIN_USB);
    assert(ryz_tool_pin_reason(39) == RYZ_TOOL_PIN_DEBUG);
    for (int pin = 22; pin <= 25; ++pin) assert(ryz_tool_pin_reason(pin) == RYZ_TOOL_PIN_INVALID);
    assert(ryz_tool_pin_reason(-1) == RYZ_TOOL_PIN_INVALID && ryz_tool_pin_reason(49) == RYZ_TOOL_PIN_INVALID);
    const char *names[] = {"available","invalid","not_exposed","memory","usb","board","debug"};
    for (int reason = 0; reason < 7; ++reason) assert(!strcmp(ryz_tool_pin_reason_name(reason),names[reason]));
    assert(!strcmp(ryz_tool_pin_reason_name(999),"invalid"));
}

static void invalid(void)
{
    uint32_t token = 99;
    assert(ryz_tool_pins_claim_pair(13,13,&token) == ESP_ERR_INVALID_ARG && !token);
    token = 99;
    assert(ryz_tool_pins_claim_pair(-1,13,&token) == ESP_ERR_INVALID_ARG && !token);
    assert(ryz_tool_pins_claim_pair(13,14,NULL) == ESP_ERR_INVALID_ARG);
    assert(ryz_tool_pins_release(0) == ESP_ERR_INVALID_ARG);
    assert(ryz_tool_pins_release(UINT32_MAX) == ESP_ERR_INVALID_STATE);
    assert(ryz_tool_pins_check_pair(13,14) == ESP_OK);
}

static atomic_int ready, go, held, successes;
static atomic_uint sequence_index;
static uint32_t tokens[40000];

static void *contender(void *unused)
{
    (void)unused;
    atomic_fetch_add(&ready,1);
    while (!atomic_load(&go)) sched_yield();
    for (int i = 0; i < 5000; ++i) {
        uint32_t token = 77;
        esp_err_t error = ryz_tool_pins_claim_pair(13,14,&token);
        if (error != ESP_OK) {
            assert(!token && (error == ESP_ERR_INVALID_STATE || error == ESP_ERR_NOT_FINISHED));
            continue;
        }
        assert(atomic_fetch_add(&held,1) == 0);
        tokens[atomic_fetch_add(&sequence_index,1)] = token;
        atomic_fetch_add(&successes,1);
        assert(atomic_fetch_sub(&held,1) == 1);
        for (int retry = 0;; ++retry) {
            assert(retry < 1000000);
            error = ryz_tool_pins_release(token);
            if (error == ESP_OK) break;
            assert(error == ESP_ERR_NOT_FINISHED);
            sched_yield();
        }
    }
    return NULL;
}
static int compare(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static void concurrency(void)
{
    pthread_t tasks[8];
    for (int i = 0; i < 8; ++i) assert(!pthread_create(&tasks[i],NULL,contender,NULL));
    while (atomic_load(&ready) != 8) sched_yield();
    atomic_store(&go,1);
    for (int i = 0; i < 8; ++i) assert(!pthread_join(tasks[i],NULL));
    assert(atomic_load(&held) == 0 && atomic_load(&successes) > 0);
    const unsigned count = atomic_load(&sequence_index);
    qsort(tokens,count,sizeof(tokens[0]),compare);
    for (unsigned i = 1; i < count; ++i) assert(tokens[i] > tokens[i-1]);
    assert(ryz_tool_pins_check_pair(13,14) == ESP_OK);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1],"pairs")) pairs();
    else if (!strcmp(argv[1],"eligibility")) eligibility();
    else if (!strcmp(argv[1],"invalid")) invalid();
    else if (!strcmp(argv[1],"concurrency")) concurrency();
    else assert(0);
    puts("TOOL_PINS_PASS");
}
