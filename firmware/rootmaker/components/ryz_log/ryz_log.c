#include "ryz_log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4) || CONFIG_LOG_VERSION != 1 || \
    !CONFIG_LOG_TIMESTAMP_SOURCE_RTOS || !CONFIG_LOG_MODE_TEXT
#error "Diagnostic capture requires audited IDF 5.5.4 text Log V1 / RTOS timestamps"
#endif

struct ryz_log_subscription {
    unsigned level;
    size_t head, used;
    uint32_t dropped, observed_gap;
    bool loss_possible;
    uint8_t bytes[RYZ_LOG_RING_BYTES];
};

extern vprintf_like_t esp_log_vprint_func;
static atomic_int s_install;
static atomic_flag s_capture = ATOMIC_FLAG_INIT;
static atomic_flag s_subscribers_lock = ATOMIC_FLAG_INIT;
static atomic_uint s_capture_gap;
static _Atomic(ryz_log_observer_fn) s_observers[4];
static ryz_log_subscription_t *s_subscriptions[RYZ_LOG_SUBSCRIPTIONS_MAX];
static _Atomic(vprintf_like_t) s_previous;

static void add_saturated(uint32_t *value, uint32_t amount)
{
    *value = UINT32_MAX - *value < amount ? UINT32_MAX : *value + amount;
}

static bool equal(const char *value, const char *literal)
{
    return value && !strncmp(value, literal, strlen(literal) + 1);
}

static const char *safe_stage(const char *value, bool system)
{
    static const char *const shell[] = {
        "system services start", "system services snapshot", "OTA image confirmation",
        "storage snapshot", "system UI render", "system UI touch", "network reprovision",
        "application catalog", "sensor worker startup", "system shell",
    };
    static const char *const stages[] = {
        "provisioning init", "provisioning start", "network snapshot", "time init",
        "time network", "time snapshot", "OTA init", "OTA prerequisites", "reprovision", "unknown",
    };
    const char *const *list = system ? stages : shell;
    const size_t count = system ? sizeof(stages)/sizeof(stages[0]) : sizeof(shell)/sizeof(shell[0]);
    for (size_t i = 0; i < count; ++i) if (equal(value, list[i])) return list[i];
    return NULL;
}

/* Preserve the pre-existing privacy boundary exactly. Match complete audited
 * V1 templates before reading variadic arguments. Only fixed labels and
 * numeric diagnostics, never dynamic error strings, SSIDs, QR payloads,
 * arbitrary tags, or unrelated application output. */
static int diagnostic(const char *format, va_list args, char *out, size_t capacity,
                        unsigned *level)
{
    enum { WORK_ERROR, WORK_REPEAT, SYSTEM_ERROR, TOUCH, GLYPH, HEALTH } kind;
    const char *tag;
    if (equal(format, LOG_FORMAT(W, "%s failed: %s (0x%x)"))) {
        kind = WORK_ERROR; tag = "ryz_workbench"; *level = ESP_LOG_WARN;
    } else if (equal(format, LOG_FORMAT(W, "%s failed: %s (0x%x); %lu repeats suppressed"))) {
        kind = WORK_REPEAT; tag = "ryz_workbench"; *level = ESP_LOG_WARN;
    } else if (equal(format, LOG_FORMAT(W, "%s: %s (0x%x); %lu repeats suppressed"))) {
        kind = SYSTEM_ERROR; tag = "ryz-system"; *level = ESP_LOG_WARN;
    } else if (equal(format, LOG_FORMAT(I, "CST816 identity: chip=0x%02x project=0x%02x fw=0x%02x factory=0x%02x"))) {
        kind = TOUCH; tag = "touch"; *level = ESP_LOG_INFO;
    } else if (equal(format, LOG_FORMAT(E, "display.text unsupported glyph at index=%u"))) {
        kind = GLYPH; tag = "ryz_display"; *level = ESP_LOG_ERROR;
    } else if (equal(format, LOG_FORMAT(I, "running image health gate: passed"))) {
        kind = HEALTH; tag = "ryz_workbench"; *level = ESP_LOG_INFO;
    } else return -1;
    (void)va_arg(args, uint32_t);
    if (!equal(va_arg(args, const char *), tag)) return -1;
    if (kind == HEALTH) return snprintf(out, capacity, "system: running image health gate passed");
    if (kind == GLYPH) {
        unsigned index = va_arg(args, unsigned);
        return snprintf(out, capacity, "display: unsupported glyph index=%u", index);
    }
    if (kind == TOUCH) {
        unsigned chip = (unsigned)va_arg(args, int), project = (unsigned)va_arg(args, int);
        unsigned firmware = (unsigned)va_arg(args, int), factory = (unsigned)va_arg(args, int);
        if (chip > 255 || project > 255 || firmware > 255 || factory > 255) return -1;
        return snprintf(out, capacity, "touch: chip=%02x project=%02x fw=%02x factory=%02x",
                        chip, project, firmware, factory);
    }
    const char *stage = safe_stage(va_arg(args, const char *), kind == SYSTEM_ERROR);
    if (!stage) return -1;
    (void)va_arg(args, const char *);
    unsigned error = va_arg(args, unsigned);
    unsigned long suppressed = kind == WORK_ERROR ? 0 : va_arg(args, unsigned long);
    return snprintf(out, capacity, "%s: error=0x%x; %lu repeats suppressed", stage, error, suppressed);
}

static void observe(const ryz_log_event_t *event)
{
    for (unsigned i = 0; i < sizeof(s_observers) / sizeof(s_observers[0]); ++i) {
        ryz_log_observer_fn observer = atomic_load_explicit(&s_observers[i], memory_order_acquire);
        if (observer) observer(event);
    }
}

static void gap(void)
{
    atomic_fetch_add_explicit(&s_capture_gap, 1, memory_order_relaxed);
    const ryz_log_event_t event = {.gap = true};
    observe(&event);
}

static void publish(const ryz_log_event_t *event)
{
    observe(event);
    if (event->filtered || !event->length) return;
    char prefix[40];
    int formatted = snprintf(prefix, sizeof(prefix), "%c (%lld) %s",
        "?EWIDV"[event->level], (long long)event->captured_ms,
        event->application ? "lua: " : "");
    size_t prefix_length = formatted > 0 && (size_t)formatted < sizeof(prefix) ? (size_t)formatted : 0;
    if (atomic_flag_test_and_set_explicit(&s_subscribers_lock, memory_order_acquire)) {
        gap();
        return;
    }
    const bool newline = event->bytes[event->length - 1] != '\n';
    const size_t needed = prefix_length + event->length + (newline ? 1 : 0);
    for (unsigned i = 0; i < RYZ_LOG_SUBSCRIPTIONS_MAX; ++i) {
        ryz_log_subscription_t *subscription = s_subscriptions[i];
        if (!subscription || event->level > subscription->level) continue;
        if (needed > RYZ_LOG_RING_BYTES - subscription->used) {
            add_saturated(&subscription->dropped, needed);
            continue; /* Reject the complete new record, never corrupt a line. */
        }
        size_t tail = (subscription->head + subscription->used) % RYZ_LOG_RING_BYTES;
        for (size_t n = 0; n < prefix_length; ++n) {
            subscription->bytes[tail] = prefix[n];
            tail = (tail + 1) % RYZ_LOG_RING_BYTES;
        }
        for (size_t n = 0; n < event->length; ++n) {
            subscription->bytes[tail] = event->bytes[n];
            tail = (tail + 1) % RYZ_LOG_RING_BYTES;
        }
        if (newline) subscription->bytes[tail] = '\n';
        subscription->used += needed;
    }
    atomic_flag_clear_explicit(&s_subscribers_lock, memory_order_release);
}

static int tap(const char *format, va_list args)
{
    if (atomic_flag_test_and_set_explicit(&s_capture, memory_order_acquire)) {
        gap();
    } else {
        const ryz_log_event_t begin = {.begin = true};
        observe(&begin);
        char bytes[RYZ_LOG_MESSAGE_MAX + 1];
        va_list capture;
        va_copy(capture, args);
        unsigned level = ESP_LOG_INFO;
        int length = diagnostic(format, capture, bytes, sizeof(bytes), &level);
        va_end(capture);
        const ryz_log_event_t event = {
            .bytes = (const uint8_t *)bytes,
            .length = length > 0 ? ((size_t)length < sizeof(bytes) ? (size_t)length : sizeof(bytes) - 1) : 0,
            .original_length = length > 0 ? (size_t)length : 0,
            .captured_ms = esp_timer_get_time() / 1000,
            .level = level, .filtered = length < 0,
        };
        publish(&event);
        atomic_flag_clear_explicit(&s_capture, memory_order_release);
    }
    /* Original sink is called exactly once, outside all capture locks. */
    va_list forward;
    va_copy(forward, args);
    vprintf_like_t previous = atomic_load_explicit(&s_previous, memory_order_acquire);
    int result = previous(format, forward);
    va_end(forward);
    return result;
}

esp_err_t ryz_log_install(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_install, &expected, 1)) {
        return expected == 2 && __atomic_load_n(&esp_log_vprint_func, __ATOMIC_SEQ_CST) == tap ?
            ESP_OK : ESP_ERR_INVALID_STATE;
    }
    /* Sole supported installer: refuse an unknown logger rather than guess
     * at recursion/ownership. The old Monitor now uses add_observer too. */
    vprintf_like_t current = __atomic_load_n(&esp_log_vprint_func, __ATOMIC_SEQ_CST);
    if (current != vprintf) {
        atomic_store(&s_install, 0);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_previous, current, memory_order_release);
    vprintf_like_t previous = esp_log_set_vprintf(tap);
    if (previous != current) {
        (void)esp_log_set_vprintf(previous);
        atomic_store(&s_install, 0);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_install, 2, memory_order_release);
    return ESP_OK;
}

esp_err_t ryz_log_add_observer(ryz_log_observer_fn observer)
{
    if (!observer) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ryz_log_install();
    if (error != ESP_OK) return error;
    for (unsigned i = 0; i < sizeof(s_observers) / sizeof(s_observers[0]); ++i)
        if (atomic_load(&s_observers[i]) == observer) return ESP_OK;
    for (unsigned i = 0; i < sizeof(s_observers) / sizeof(s_observers[0]); ++i) {
        ryz_log_observer_fn empty = NULL;
        if (atomic_compare_exchange_strong(&s_observers[i], &empty, observer)) return ESP_OK;
        if (empty == observer) return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t ryz_log_subscribe(unsigned maximum_level, ryz_log_subscription_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (maximum_level < ESP_LOG_ERROR || maximum_level > ESP_LOG_VERBOSE) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ryz_log_install();
    if (error != ESP_OK) return error;
    ryz_log_subscription_t *subscription = heap_caps_calloc(1, sizeof(*subscription),
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!subscription) return ESP_ERR_NO_MEM;
    subscription->level = maximum_level;
    subscription->observed_gap = atomic_load(&s_capture_gap);
    if (atomic_flag_test_and_set_explicit(&s_subscribers_lock, memory_order_acquire)) {
        heap_caps_free(subscription);
        return ESP_ERR_NOT_FINISHED;
    }
    error = ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < RYZ_LOG_SUBSCRIPTIONS_MAX; ++i) {
        if (!s_subscriptions[i]) {
            s_subscriptions[i] = subscription;
            *out = subscription;
            error = ESP_OK;
            break;
        }
    }
    atomic_flag_clear_explicit(&s_subscribers_lock, memory_order_release);
    if (error != ESP_OK) heap_caps_free(subscription);
    return error;
}

esp_err_t ryz_log_read(ryz_log_subscription_t *subscription, uint8_t *bytes,
                      size_t capacity, size_t *length, uint32_t *dropped_bytes,
                      bool *loss_possible)
{
    if (!subscription || !bytes || !capacity || capacity > RYZ_LOG_MESSAGE_MAX ||
        !length || !dropped_bytes || !loss_possible) return ESP_ERR_INVALID_ARG;
    *length = 0;
    *dropped_bytes = 0;
    *loss_possible = false;
    if (atomic_flag_test_and_set_explicit(&s_subscribers_lock, memory_order_acquire)) return ESP_ERR_NOT_FINISHED;
    bool found = false;
    for (unsigned i = 0; i < RYZ_LOG_SUBSCRIPTIONS_MAX; ++i)
        if (s_subscriptions[i] == subscription) { found = true; break; }
    if (found) {
        uint32_t observed = atomic_load(&s_capture_gap);
        if (observed != subscription->observed_gap) subscription->loss_possible = true;
        subscription->observed_gap = observed;
        size_t count = subscription->used < capacity ? subscription->used : capacity;
        for (size_t i = 0; i < count; ++i) {
            bytes[i] = subscription->bytes[subscription->head];
            subscription->head = (subscription->head + 1) % RYZ_LOG_RING_BYTES;
        }
        subscription->used -= count;
        *length = count;
        *dropped_bytes = subscription->dropped;
        *loss_possible = subscription->loss_possible;
    }
    atomic_flag_clear_explicit(&s_subscribers_lock, memory_order_release);
    return found ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ryz_log_unsubscribe(ryz_log_subscription_t *subscription)
{
    if (!subscription) return ESP_ERR_INVALID_ARG;
    if (atomic_flag_test_and_set_explicit(&s_subscribers_lock, memory_order_acquire)) return ESP_ERR_NOT_FINISHED;
    bool found = false;
    for (unsigned i = 0; i < RYZ_LOG_SUBSCRIPTIONS_MAX; ++i) {
        if (s_subscriptions[i] == subscription) {
            s_subscriptions[i] = NULL;
            found = true;
            break;
        }
    }
    atomic_flag_clear_explicit(&s_subscribers_lock, memory_order_release);
    if (found) heap_caps_free(subscription);
    return found ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ryz_log_write(unsigned level, const uint8_t *bytes, size_t length)
{
    if (level < ESP_LOG_ERROR || level > ESP_LOG_VERBOSE || !bytes || !length || length > RYZ_LOG_MESSAGE_MAX)
        return ESP_ERR_INVALID_ARG;
    const ryz_log_event_t event = {
        .bytes = bytes, .length = length, .original_length = length,
        .captured_ms = esp_timer_get_time() / 1000, .level = level, .application = true,
    };
    publish(&event);
    ESP_LOG_LEVEL_LOCAL(level, "lua", "%.*s", (int)length, (const char *)bytes);
    return ESP_OK;
}
