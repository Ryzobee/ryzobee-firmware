#include "ryz_monitor_source.h"
#include "ryz_monitor_stream.h"
#include "esp_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned setter_calls, forward_calls;
static char forwarded[1024];
static void (*clock_hook)(void);
int monitor_test_vprintf(const char *format, va_list args)
{
    ++forward_calls;
    vsnprintf(forwarded, sizeof(forwarded), format, args);
    return 712;
}
vprintf_like_t esp_log_vprint_func = monitor_test_vprintf;
vprintf_like_t esp_log_set_vprintf(vprintf_like_t callback)
{
    ++setter_calls;
    vprintf_like_t old = esp_log_vprint_func;
    esp_log_vprint_func = callback;
    return old;
}
int64_t esp_timer_get_time(void)
{
    if (clock_hook) {
        void (*hook)(void) = clock_hook;
        clock_hook = NULL;
        hook();
    }
    return 123456000;
}
static int emit(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = esp_log_vprint_func(format, args);
    va_end(args);
    return result;
}
static int foreign_sink(const char *format, va_list args)
{
    (void)format; (void)args;
    return -91;
}
static void health(void)
{
    assert(emit(LOG_FORMAT(I, "running image health gate: passed"),
                (uint32_t)42, "ryz_workbench") == 712);
}
static ryz_monitor_stream_info_t info(void)
{
    ryz_monitor_stream_info_t out;
    assert(ryz_monitor_stream_info(&out) == ESP_OK);
    return out;
}
static ryz_monitor_page_t page(void)
{
    ryz_monitor_stream_info_t status = info();
    ryz_monitor_page_t out;
    assert(ryz_monitor_stream_page(status.session_id, status.view_generation, 0, &out) == ESP_OK);
    return out;
}
static void active(void)
{
    assert(ryz_monitor_log_install() == ESP_OK);
    assert(ryz_monitor_stream_reset(1, 1, RYZ_MONITOR_SYSTEM) == ESP_OK);
    assert(ryz_monitor_stream_accept(1, true) == ESP_OK);
    ryz_monitor_log_capture(true);
}
static void record_is(const ryz_monitor_record_t *record, const char *value)
{
    assert(record->length == strlen(value));
    assert(!memcmp(record->bytes, value, record->length));
    assert(record->captured_ms == 123456);
    assert(!record->truncated);
}
static void test_install(void)
{
    assert(ryz_monitor_log_install() == ESP_OK);
    assert(ryz_monitor_log_install() == ESP_OK && setter_calls == 1);
    assert(emit("raw:%s:%u", "fixture", 81U) == 712);
    assert(!strcmp(forwarded, "raw:fixture:81") && forward_calls == 1);
    ryz_monitor_stream_info_t out;
    assert(ryz_monitor_stream_info(&out) == ESP_ERR_INVALID_STATE);
}
static void test_ownership(void)
{
    esp_log_vprint_func = foreign_sink;
    assert(ryz_monitor_log_install() == ESP_ERR_INVALID_STATE);
    assert(setter_calls == 0 && esp_log_vprint_func == foreign_sink);
    esp_log_vprint_func = monitor_test_vprintf;
    assert(ryz_monitor_log_install() == ESP_OK);
    esp_log_vprint_func = foreign_sink;
    assert(ryz_monitor_log_install() == ESP_ERR_INVALID_STATE);
    assert(setter_calls == 1 && esp_log_vprint_func == foreign_sink);
}
static void test_allowed(void)
{
    active();
    assert(emit(LOG_FORMAT(W, "%s failed: %s (0x%x)"), (uint32_t)42, "ryz_workbench",
                "system services start", "fixture-error-text", 0x123U) == 712);
    assert(!strcmp(forwarded, "W (42) ryz_workbench: system services start failed: fixture-error-text (0x123)\n"));
    emit(LOG_FORMAT(W, "%s failed: %s (0x%x); %lu repeats suppressed"), (uint32_t)42,
         "ryz_workbench", "system UI touch", "fixture-error-text", 0x456U, 12UL);
    emit(LOG_FORMAT(W, "%s: %s (0x%x); %lu repeats suppressed"), (uint32_t)42,
         "ryz-system", "time init", "fixture-error-text", 0x789U, 3UL);
    emit(LOG_FORMAT(I, "CST816 identity: chip=0x%02x project=0x%02x fw=0x%02x factory=0x%02x"),
         (uint32_t)42, "touch", 1, 2, 3, 255);
    emit(LOG_FORMAT(E, "display.text unsupported glyph at index=%u"), (uint32_t)42,
         "ryz_display", 7U);
    health();
    ryz_monitor_page_t out = page();
    assert(out.count == 6 && out.stream.filtered_logs == 0 && forward_calls == 6);
    record_is(&out.records[0], "system services start: error=0x123; 0 repeats suppressed");
    record_is(&out.records[1], "system UI touch: error=0x456; 12 repeats suppressed");
    record_is(&out.records[2], "time init: error=0x789; 3 repeats suppressed");
    record_is(&out.records[3], "touch: chip=01 project=02 fw=03 factory=ff");
    record_is(&out.records[4], "display: unsupported glyph index=7");
    record_is(&out.records[5], "system: running image health gate passed");
}
static void test_privacy(void)
{
    active();
    emit("{\"op\":\"wifi\",\"password\":\"fixture-sensitive\"}");
    emit(LOG_FORMAT(I, "AP SSID=%s"), (uint32_t)42, "ryz-network", "fixture-sensitive");
    emit(LOG_FORMAT(W, "%s failed: %s (0x%x)"), (uint32_t)42, "untrusted-tag",
         "system UI touch", "fixture-sensitive", 0U);
    emit(LOG_FORMAT(W, "%s failed: %s (0x%x)"), (uint32_t)42, "ryz_workbench",
         "fixture-sensitive", "fixture-sensitive", 0U);
    emit(LOG_FORMAT(I, "CST816 identity: chip=0x%02x project=0x%02x fw=0x%02x factory=0x%02x"),
         (uint32_t)42, "touch", 300, 2, 3, 4);
    assert(info().filtered_logs == 5 && page().count == 0);
    emit(LOG_FORMAT(W, "%s failed: %s (0x%x)"), (uint32_t)42, "ryz_workbench",
         "system UI touch", "fixture-sensitive", 3U);
    ryz_monitor_page_t out = page();
    assert(out.count == 1 && forward_calls == 6);
    record_is(&out.records[0], "system UI touch: error=0x3; 0 repeats suppressed");
}
static void clear_inflight(void)
{
    uint32_t generation;
    assert(ryz_monitor_stream_clear(1, info().view_generation, &generation) == ESP_OK);
}
static void stop_inflight(void)
{
    assert(ryz_monitor_stream_accept(1, false) == ESP_OK);
}
static void switch_inflight(void)
{
    stop_inflight();
    assert(ryz_monitor_stream_reconfigure(1, 2, RYZ_MONITOR_UART) == ESP_OK);
}
static void test_stale(void)
{
    active();
    clock_hook = clear_inflight; health();
    assert(page().count == 0);
    clock_hook = stop_inflight; health();
    assert(page().count == 0);
    assert(ryz_monitor_stream_accept(1, true) == ESP_OK);
    clock_hook = switch_inflight; health();
    assert(page().count == 0 && info().source == RYZ_MONITOR_UART);
    health();
    assert(page().count == 0 && info().filtered_logs == 0 && forward_calls == 4);
}
static void test_pause(void)
{
    active(); health();
    uint32_t generation;
    assert(ryz_monitor_stream_pause(1, info().view_generation, true, &generation) == ESP_OK);
    health();
    assert(page().count == 1 && info().chunks == 2);
    assert(ryz_monitor_stream_pause(1, generation, false, &generation) == ESP_OK);
    assert(page().count == 2);
    ryz_monitor_log_capture(false); health();
    assert(page().count == 2 && forward_calls == 3);
}
static void test_reentrant(void)
{
    active();
    clock_hook = health;
    health();
    assert(page().count == 1 && info().capture_gap_since_boot);
    assert(forward_calls == 2);
    health();
    assert(page().count == 2 && forward_calls == 3);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "install")) test_install();
    else if (!strcmp(argv[1], "ownership")) test_ownership();
    else if (!strcmp(argv[1], "allowed")) test_allowed();
    else if (!strcmp(argv[1], "privacy")) test_privacy();
    else if (!strcmp(argv[1], "stale")) test_stale();
    else if (!strcmp(argv[1], "pause")) test_pause();
    else if (!strcmp(argv[1], "reentrant")) test_reentrant();
    else assert(!"unknown test");
    puts("MONITOR_LOG_PASS");
    return 0;
}
