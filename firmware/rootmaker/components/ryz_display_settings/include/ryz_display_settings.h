#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "touch_protocol.h"

typedef struct {
    uint8_t rotation;    /* Clockwise quarter turns, 0..3. */
    uint8_t sleep_index; /* 15s, 30s, 60s, 300s, Never. */
    uint8_t brightness;  /* 10..100, multiples of 5; linear PWM, not calibrated light. */
} ryz_display_settings_value_t;

typedef enum {
    RYZ_DISPLAY_SETTINGS_LOADING,
    RYZ_DISPLAY_SETTINGS_READY,
    RYZ_DISPLAY_SETTINGS_SAVING,
    RYZ_DISPLAY_SETTINGS_APPLYING,
    RYZ_DISPLAY_SETTINGS_SAVED,
    RYZ_DISPLAY_SETTINGS_FAILED,
    RYZ_DISPLAY_SETTINGS_APPLY_FAILED,
    RYZ_DISPLAY_SETTINGS_UNKNOWN,
} ryz_display_settings_phase_t;

typedef struct {
    uint64_t revision;
    uint64_t operation_id;
    ryz_display_settings_phase_t phase;
    ryz_display_settings_value_t confirmed;
    ryz_display_settings_value_t requested;
    bool ready;
    bool persisted;       /* The complete requested tuple is confirmed in NVS. */
    bool asleep;
    bool input_blocked;   /* Incomplete apply or uncertain backlight blocks input. */
    esp_err_t error;
    esp_err_t load_error; /* Missing/corrupt config falls back; never reported SAVED. */
    esp_err_t preview_error; /* Temporary PWM/restore failure, never a persistence result. */
} ryz_display_settings_snapshot_t;

bool ryz_display_settings_valid(ryz_display_settings_value_t value);
bool ryz_display_settings_equal(ryz_display_settings_value_t a, ryz_display_settings_value_t b);
ryz_display_settings_value_t ryz_display_settings_defaults(void);
uint32_t ryz_display_settings_timeout_ms(uint8_t sleep_index);

/* Bootstrap/main only, before the panel/first frame and service workers.
 * Initializes NVS and synchronously reads the same checked tuple as the worker.
 * Always leaves defaults on any error (including missing preferences), returning
 * the original load error. Read-only: no erase/write, worker creation or panel IO. */
esp_err_t ryz_display_settings_load_boot(ryz_display_settings_value_t *out);
/* Call after nvs_flash_init. Creates one bounded persistence worker; no panel
 * IO. A successful start does not imply configuration loaded/applied. */
esp_err_t ryz_display_settings_start(void);
/* Copy/intent only. Nonzero revision must match the snapshot actually shown.
 * RETRY after UNKNOWN first reads the complete stored tuple; never auto-retry.
 * Success admits one request, not persistence/application or SAVED. */
esp_err_t ryz_display_settings_get_snapshot(ryz_display_settings_snapshot_t *out);
esp_err_t ryz_display_settings_submit(uint64_t expected_revision, ryz_display_settings_value_t value);
/* UI Owner only, copied/coalesced intents. Preview changes brightness only,
 * never NVS, the confirmed tuple, orientation or idle timeout. Successful
 * admission does not mean PWM has completed; owner_tick reports any failure.
 * Normal preview does not revise the snapshot or cancel an ongoing drag. */
esp_err_t ryz_display_settings_preview(uint64_t expected_revision, uint8_t brightness);
/* Owner route/reset/discard cleanup; always queues restoration to the latest
 * confirmed value, including when a previously submitted SAVE is in flight. */
void ryz_display_settings_end_preview(void);
/* Sole SPI/input Owner, after ryz_display_init. Call during boot with false;
 * use true only after boot completion (false also prevents fault-page sleep).
 * allow_configuration_apply must be false while private QR/canvas cleanup is
 * outstanding: retains queued results and never clears a privacy barrier.
 * Applies worker results and idle backlight policy; no NVS access here. */
bool ryz_display_settings_owner_tick(uint64_t now_ms, bool allow_idle_sleep,
                                      bool allow_configuration_apply);
/* Sole physical input Owner, immediately after EVERY ryz_touch_read and
 * before system/Lua routing. True consumes the sample. Wake swallows the
 * complete contact through physical release; read errors never fake release. */
bool ryz_display_settings_filter_touch(ryz_touch_sample_t *sample, esp_err_t error, uint64_t now_ms);
