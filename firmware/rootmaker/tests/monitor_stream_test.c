#include "ryz_monitor_stream.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void open_stream(void)
{
    assert(ryz_monitor_stream_reset(1, 1, RYZ_MONITOR_SYSTEM) == ESP_OK);
    assert(ryz_monitor_stream_accept(1, true) == ESP_OK);
}
static ryz_monitor_capture_token_t token(void)
{
    ryz_monitor_capture_token_t value;
    assert(ryz_monitor_stream_token(&value) == ESP_OK);
    return value;
}
static ryz_monitor_stream_info_t info(void)
{
    ryz_monitor_stream_info_t value;
    assert(ryz_monitor_stream_info(&value) == ESP_OK);
    return value;
}
static void append(unsigned value)
{
    uint8_t data[3] = {(uint8_t)value, 0, (uint8_t)(value ^ 0xff)};
    assert(ryz_monitor_stream_append(token(), data, sizeof(data), sizeof(data), value) == ESP_OK);
}
static void invalid(void)
{
    ryz_monitor_stream_info_t value;
    memset(&value, 0x7f, sizeof(value));
    assert(ryz_monitor_stream_info(&value) == ESP_ERR_INVALID_STATE && !value.session_id);
    assert(ryz_monitor_stream_reset(0,1,RYZ_MONITOR_SYSTEM) == ESP_ERR_INVALID_ARG);
    assert(ryz_monitor_stream_reset(1,0,RYZ_MONITOR_SYSTEM) == ESP_ERR_INVALID_ARG);
    open_stream();
    assert(ryz_monitor_stream_reset(2,1,RYZ_MONITOR_SYSTEM) == ESP_ERR_INVALID_STATE);
    const ryz_monitor_capture_token_t t = token();
    assert(ryz_monitor_stream_append(t,NULL,1,1,0) == ESP_ERR_INVALID_ARG);
    assert(ryz_monitor_stream_append(t,"a",1,0,0) == ESP_ERR_INVALID_ARG);
    assert(ryz_monitor_stream_append(t,"a",1,1,-1) == ESP_ERR_INVALID_ARG);
    assert(!info().chunks);
    uint32_t next = 7;
    assert(ryz_monitor_stream_pause(9,1,true,&next) == ESP_ERR_INVALID_STATE && next == 0);
    assert(ryz_monitor_stream_clear(1,0,&next) == ESP_ERR_INVALID_ARG && next == 0);
    assert(ryz_monitor_stream_accept(1,false) == ESP_OK);
    assert(ryz_monitor_stream_append(t,"a",1,1,0) == ESP_ERR_INVALID_STATE);
    assert(ryz_monitor_stream_reset(1,1,RYZ_MONITOR_SYSTEM) == ESP_ERR_INVALID_STATE);
}
static void retention(void)
{
    open_stream();
    for (unsigned i=1; i<=40; ++i) append(i);
    const ryz_monitor_stream_info_t value = info();
    assert(value.chunks == 40 && value.bytes == 120 && value.retained_records == 32);
    assert(value.overwritten_chunks == 8 && value.overwritten_bytes == 24);
    assert(value.first_sequence == 9 && value.last_sequence == 40);
    ryz_monitor_page_t a, b;
    assert(ryz_monitor_stream_page(1,1,1,&a) == ESP_OK && a.gap && a.more);
    assert(a.count == 8 && a.next_sequence == 16);
    assert(ryz_monitor_stream_page(1,1,1,&b) == ESP_OK && !memcmp(&a,&b,sizeof(a)));
    for (unsigned i=0; i<a.count; ++i) {
        assert(a.records[i].sequence == i+9 && a.records[i].captured_ms == i+9);
        assert(a.records[i].length == 3 && a.records[i].bytes[1] == 0);
    }
    assert(ryz_monitor_stream_page(1,1,0,&a) == ESP_OK && !a.gap);
    assert(ryz_monitor_stream_page(1,1,40,&a) == ESP_OK && !a.count && !a.more);
    assert(ryz_monitor_stream_page(1,1,41,&a) == ESP_ERR_INVALID_ARG && !a.count);
}
static void pause_test(void)
{
    open_stream();
    for (unsigned i=1; i<=8; ++i) append(i);
    uint32_t generation;
    assert(ryz_monitor_stream_pause(1,1,true,&generation) == ESP_OK && generation == 2);
    ryz_monitor_page_t frozen, later;
    assert(ryz_monitor_stream_page(1,2,0,&frozen) == ESP_OK);
    for (unsigned i=9; i<=100; ++i) append(i);
    assert(ryz_monitor_stream_page(1,2,0,&later) == ESP_OK && later.count == 8);
    assert(!memcmp(frozen.records,later.records,sizeof(frozen.records)));
    assert(info().chunks == 100 && info().retained_records == 8 && info().last_sequence == 8);
    assert(ryz_monitor_stream_pause(1,2,false,&generation) == ESP_OK && generation == 3);
    assert(info().first_sequence == 69 && info().last_sequence == 100 && !info().paused);
    assert(ryz_monitor_stream_page(1,2,0,&later) == ESP_ERR_INVALID_STATE);
}
static void clear_test(void)
{
    open_stream(); append(1);
    const ryz_monitor_capture_token_t before = token();
    uint32_t generation;
    assert(ryz_monitor_stream_pause(1,1,true,&generation) == ESP_OK);
    assert(ryz_monitor_stream_clear(1,2,&generation) == ESP_OK && generation == 3);
    assert(ryz_monitor_stream_append(before,"old",3,3,2) == ESP_ERR_INVALID_STATE);
    assert(info().paused && !info().retained_records && info().chunks == 1);
    append(2);
    assert(!info().retained_records && info().chunks == 2);
    assert(ryz_monitor_stream_pause(1,3,false,&generation) == ESP_OK);
    assert(info().first_sequence == 2 && info().retained_records == 1);
}
static void sources(void)
{
    open_stream(); append(1);
    ryz_monitor_capture_token_t old = token();
    assert(ryz_monitor_stream_reconfigure(1,2,RYZ_MONITOR_UART) == ESP_ERR_INVALID_STATE);
    assert(ryz_monitor_stream_accept(1,false) == ESP_OK);
    assert(ryz_monitor_stream_reconfigure(1,2,RYZ_MONITOR_UART) == ESP_OK);
    assert(info().accepting && !info().retained_records && info().view_generation == 2);
    assert(ryz_monitor_stream_append(old,"old",3,3,1) == ESP_ERR_INVALID_STATE);
    ryz_monitor_capture_token_t current = token();
    assert(current.source == RYZ_MONITOR_UART);
    ryz_monitor_capture_token_t fake = current; fake.source = RYZ_MONITOR_SYSTEM;
    assert(ryz_monitor_stream_append(fake,"bad",3,3,1) == ESP_ERR_INVALID_STATE);
    assert(ryz_monitor_stream_append(current,"\0u",2,2,2) == ESP_OK);
    assert(ryz_monitor_stream_accept(1,false) == ESP_OK);
    assert(ryz_monitor_stream_reset(4,2,RYZ_MONITOR_SYSTEM) == ESP_OK);
    assert(ryz_monitor_stream_accept(4,true) == ESP_OK);
    assert(ryz_monitor_stream_append(current,"old",3,3,3) == ESP_ERR_INVALID_STATE);
    assert(!info().chunks && info().session_id == 4);
}
static void counts(void)
{
    open_stream();
    uint8_t large[RYZ_MONITOR_RECORD_BYTES]; memset(large,0x7f,sizeof(large));
    assert(ryz_monitor_stream_append(token(),large,sizeof(large),400,2) == ESP_OK);
    assert(ryz_monitor_stream_filtered(token()) == ESP_OK);
    assert(info().bytes == 400 && info().truncated_bytes == 304 && info().filtered_logs == 1);
    assert(info().chunks == 1 && info().overwritten_bytes == 0);
    ryz_monitor_stream_gap();
    assert(info().capture_gap_since_boot);
    uint32_t generation;
    assert(ryz_monitor_stream_clear(1,1,&generation) == ESP_OK);
    assert(info().capture_gap_since_boot && info().truncated_bytes == 304);
}
static void empty_pause(void)
{
    open_stream();
    uint32_t generation;
    assert(ryz_monitor_stream_pause(1,1,true,&generation) == ESP_OK);
    append(1);
    ryz_monitor_page_t page;
    assert(ryz_monitor_stream_page(1,generation,1,&page) == ESP_ERR_INVALID_ARG);
    assert(ryz_monitor_stream_page(1,generation,0,&page) == ESP_OK && !page.count);
}
static atomic_uint successes, done;
static void *produce(void *arg)
{
    uint8_t bytes[96]; memset(bytes,(int)(uintptr_t)arg,sizeof(bytes));
    for (unsigned i=0; i<4000; ++i) {
        ryz_monitor_capture_token_t t;
        esp_err_t error = ryz_monitor_stream_token(&t);
        if (error == ESP_ERR_NOT_FINISHED) continue;
        assert(error == ESP_OK);
        error = ryz_monitor_stream_append(t,bytes,sizeof(bytes),sizeof(bytes),i);
        assert(error == ESP_OK || error == ESP_ERR_NOT_FINISHED);
        if (error == ESP_OK) atomic_fetch_add(&successes,1);
    }
    atomic_fetch_add(&done,1);
    return NULL;
}
static void concurrency(void)
{
    open_stream();
    pthread_t threads[4];
    for (uintptr_t i=0; i<4; ++i) assert(!pthread_create(&threads[i],NULL,produce,(void *)(i+1)));
    while (atomic_load(&done) != 4) {
        ryz_monitor_page_t page;
        esp_err_t error = ryz_monitor_stream_page(1,1,0,&page);
        assert(error == ESP_OK || error == ESP_ERR_NOT_FINISHED);
        if (error == ESP_OK) for (unsigned i=0; i<page.count; ++i) {
            assert(page.records[i].length == 96);
            for (unsigned j=0; j<96; ++j) assert(page.records[i].bytes[j] == page.records[i].bytes[0]);
            if (i) assert(page.records[i].sequence == page.records[i-1].sequence + 1);
        }
        sched_yield();
    }
    for (unsigned i=0; i<4; ++i) assert(!pthread_join(threads[i],NULL));
    assert(info().chunks == atomic_load(&successes) && info().chunks > 0);
    assert(info().bytes == info().chunks * 96 && info().retained_records <= 32);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1],"invalid")) invalid();
    else if (!strcmp(argv[1],"retention")) retention();
    else if (!strcmp(argv[1],"pause")) pause_test();
    else if (!strcmp(argv[1],"clear")) clear_test();
    else if (!strcmp(argv[1],"sources")) sources();
    else if (!strcmp(argv[1],"counts")) counts();
    else if (!strcmp(argv[1],"empty_pause")) empty_pause();
    else if (!strcmp(argv[1],"concurrency")) concurrency();
    else assert(false);
    puts("MONITOR_STREAM_PASS");
}
