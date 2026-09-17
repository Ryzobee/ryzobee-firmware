#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Generic bounded diagnostic source, independent of tools, Lua and UI.
 * System capture is a fixed numeric/template allowlist, not raw firmware
 * log access. Explicit application writes are labelled separately.
 * Installing never changes log levels or suppresses the console output. */
#define RYZ_LOG_MESSAGE_MAX 256U
#define RYZ_LOG_SUBSCRIPTIONS_MAX 16U
#define RYZ_LOG_RING_BYTES 1024U

typedef struct ryz_log_subscription ryz_log_subscription_t;
typedef struct {
    const uint8_t *bytes;
    size_t length, original_length;
    int64_t captured_ms;
    unsigned level;
    bool filtered, gap, application, begin;
} ryz_log_event_t;

/* Trusted C observer: boot-lifetime, max four distinct callbacks. Never
 * retain event bytes, log from the callback, block, or call Lua here. The
 * callback can run concurrently on producer tasks; filtered/gap/begin events
 * have no data. A serialized system capture emits begin then one final
 * event; a reader can capture its own admission generation at begin, so a
 * clear/stop during formatting cannot admit a stale record afterwards.
 * Application and gap notifications may interleave with those two phases. */
typedef void (*ryz_log_observer_fn)(const ryz_log_event_t *event);
esp_err_t ryz_log_add_observer(ryz_log_observer_fn observer);
esp_err_t ryz_log_install(void);

/* Each subscriber owns a 1024-byte PSRAM ring, allocated only on open.
 * Admission never waits. read/unsubscribe return NOT_FINISHED on contention;
 * failed unsubscribe retains ownership. read is nonblocking. dropped_bytes
 * is known discarded output bytes (including appended newlines), not an
 * estimate of unobserved input. loss_possible covers capture contention. */
esp_err_t ryz_log_subscribe(unsigned maximum_level, ryz_log_subscription_t **out);
esp_err_t ryz_log_read(ryz_log_subscription_t *subscription, uint8_t *bytes,
                      size_t capacity, size_t *length, uint32_t *dropped_bytes,
                      bool *loss_possible);
esp_err_t ryz_log_unsubscribe(ryz_log_subscription_t *subscription);
esp_err_t ryz_log_write(unsigned level, const uint8_t *bytes, size_t length);
