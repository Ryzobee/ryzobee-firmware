#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "ryz_apps.h"

/* Synchronous, read-only Store access; call from the Apps command owner, never
 * the UI Owner. expected_revision must be nonzero and match a ready Store with
 * no pending recovery both before and after reading. No source is retained.
 *
 * source_valid means the Store's byte-size/NUL constraints passed, not that
 * Lua syntax, metadata or application behavior was validated. Invalid source
 * may still return ESP_OK with a verified raw file identity for later CAS
 * deletion. Metadata remains empty in that case and source_error records the
 * original INVALID_ARG/INVALID_SIZE. INVALID_SIZE requires a raw size of zero
 * or above SOURCE_MAX; INVALID_ARG requires 1..SOURCE_MAX bytes. An inconsistent
 * size/error pair or a changed identity/error classification
 * returns INVALID_STATE; other Store errors propagate. All failures clear out
 * when it is non-NULL. name may point into a previous value of out.
 */
esp_err_t ryz_apps_read_detail(const char *name, uint32_t expected_revision,
                               ryz_apps_detail_t *out);
