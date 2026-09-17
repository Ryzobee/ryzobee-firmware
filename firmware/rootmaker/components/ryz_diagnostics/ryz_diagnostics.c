#include "ryz_diagnostics.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#if defined(RYZ_DIAGNOSTICS_HOST_TEST)
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
extern int64_t ryz_diagnostics_test_time(void);
extern void ryz_diagnostics_test_random(void *, size_t);
#define NOW() ryz_diagnostics_test_time()
#define RANDOM(p, n) ryz_diagnostics_test_random(p, n)
#else
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)
#define NOW() esp_timer_get_time()
#define RANDOM(p, n) esp_fill_random(p, n)
#endif

#define REPORT_TTL_US INT64_C(600000000)
static ryz_diagnostics_snapshot_t s_report;
static char s_capability[33];
static int64_t s_expires;
static uint32_t s_sequence;
static bool s_valid;

static bool copy_text(char *out, size_t capacity, const char *input, bool *sanitized)
{
    if (input == NULL) input = "";
    size_t available = strnlen(input, capacity), offset = 0, written = 0;
    while (offset < available && written < capacity - 1) {
        unsigned char lead = (unsigned char)input[offset];
        size_t count = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (count != 0 && (count > available - offset || count > capacity - 1 - written)) {
            if (available == capacity) break;
            count = 0;
        }
        bool valid = count != 0;
        for (size_t i = 1; valid && i < count; ++i)
            valid = ((unsigned char)input[offset + i] & 0xc0U) == 0x80U;
        if (valid && count >= 3) {
            unsigned char second = (unsigned char)input[offset + 1];
            valid = !(lead == 0xe0 && second < 0xa0) && !(lead == 0xed && second >= 0xa0) &&
                !(lead == 0xf0 && second < 0x90) && !(lead == 0xf4 && second >= 0x90);
        }
        if (!valid) {
            out[written++] = '?'; ++offset; *sanitized = true;
        } else {
            memcpy(out + written, input + offset, count); written += count; offset += count;
        }
    }
    out[written] = '\0';
    return available == capacity || offset < available;
}

static bool live_locked(int64_t now)
{
    if (!s_valid || now < 0 || now >= s_expires) return false;
    return true;
}

esp_err_t ryz_diagnostics_publish(const ryz_diagnostics_input_t *input)
{
    if (input == NULL) return ESP_ERR_INVALID_ARG;
    ryz_diagnostics_snapshot_t report = {0};
    uint8_t entropy[16];
    char capability[33];
    RANDOM(entropy, sizeof(entropy));
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(entropy); ++i) {
        capability[i * 2] = digits[entropy[i] >> 4];
        capability[i * 2 + 1] = digits[entropy[i] & 15];
    }
    capability[32] = '\0';
#define COPY(field) (void)copy_text(report.field, sizeof(report.field), input->field, &report.text_sanitized)
    COPY(phase); COPY(code); COPY(summary); COPY(source_file); COPY(job_id); COPY(firmware_version);
#undef COPY
    report.error_truncated = copy_text(report.error, sizeof(report.error), input->error, &report.text_sanitized) || input->error_truncated;
    report.output_truncated = copy_text(report.output, sizeof(report.output), input->output, &report.text_sanitized) || input->output_truncated;
    report.line = input->line;
    report.duration_ms = input->duration_ms;
    report.peak_bytes = input->peak_bytes;
    report.lua_ok = input->lua_ok;
    report.ok = input->ok;
    report.cleanup_ok = input->cleanup_ok < 0 ? -1 : !!input->cleanup_ok;
    int64_t now = NOW();
    if (now < 0 || now > INT64_MAX - REPORT_TTL_US) return ESP_ERR_INVALID_STATE;
    LOCK();
    ++s_sequence;
    snprintf(report.id, sizeof(report.id), "%08" PRIx32, s_sequence);
    s_report = report;
    memcpy(s_capability, capability, sizeof(s_capability));
    s_expires = now + REPORT_TTL_US;
    s_valid = true;
    UNLOCK();
    memset(entropy, 0, sizeof(entropy));
    memset(capability, 0, sizeof(capability));
    return ESP_OK;
}

void ryz_diagnostics_clear(void)
{
    LOCK();
    s_valid = false;
    memset(&s_report, 0, sizeof(s_report));
    memset(s_capability, 0, sizeof(s_capability));
    s_expires = 0;
    UNLOCK();
}

esp_err_t ryz_diagnostics_snapshot(const char *id, const char *capability,
                                 ryz_diagnostics_snapshot_t *output)
{
    if (output == NULL) return ESP_ERR_INVALID_ARG;
    memset(output, 0, sizeof(*output));
    if (id == NULL || capability == NULL || strnlen(id, 9) != 8 ||
        strnlen(capability, 33) != 32) return ESP_ERR_NOT_FOUND;
    int64_t now = NOW();
    LOCK();
    unsigned different = 0;
    for (size_t i = 0; i < 32; ++i) different |= (unsigned char)capability[i] ^ (unsigned char)s_capability[i];
    bool match = live_locked(now) && different == 0 && memcmp(id, s_report.id, 8) == 0;
    if (match) *output = s_report;
    UNLOCK();
    return match ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t ryz_diagnostics_get_path(char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return ESP_ERR_INVALID_ARG;
    output[0] = '\0';
    int64_t now = NOW();
    LOCK();
    bool valid = live_locked(now);
    int length = valid ? snprintf(output, capacity, "/f/%s?cap=%s", s_report.id, s_capability) : 0;
    UNLOCK();
    if (!valid) return ESP_ERR_NOT_FOUND;
    if (length < 0 || (size_t)length >= capacity) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
