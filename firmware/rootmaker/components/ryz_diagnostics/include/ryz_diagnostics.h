#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* A single volatile, read-only fault report. Publish only after job cleanup.
 * Unknown numbers use -1. Text is copied immediately, never retained by pointer.
 * Caller must scrub secrets: this is local HTTP, not an encrypted export. */
typedef struct {
    const char *phase, *code, *summary, *source_file, *job_id;
    const char *firmware_version, *error, *output;
    int32_t line;
    int64_t duration_ms, peak_bytes;
    bool lua_ok, ok;
    int cleanup_ok; /* -1 unknown, 0 failed, 1 passed */
    bool error_truncated, output_truncated;
} ryz_diagnostics_input_t;

typedef struct {
    char id[9], phase[24], code[24], summary[96], source_file[41];
    char job_id[32], firmware_version[48], error[256], output[513];
    int32_t line;
    int64_t duration_ms, peak_bytes;
    bool lua_ok, ok;
    int cleanup_ok;
    bool error_truncated, output_truncated;
    bool text_sanitized; /* Invalid UTF-8 bytes replaced with '?' for valid JSON. */
} ryz_diagnostics_snapshot_t;

esp_err_t ryz_diagnostics_publish(const ryz_diagnostics_input_t *input);
void ryz_diagnostics_clear(void);
/* No latest-report API: every read is bound to id + 128-bit capability. */
esp_err_t ryz_diagnostics_snapshot(const char *id, const char *capability,
                                 ryz_diagnostics_snapshot_t *output);
/* Relative path only. Caller must separately verify the protected AP + server. */
esp_err_t ryz_diagnostics_get_path(char *output, size_t capacity);

extern const char ryz_diagnostics_html[];
