#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/* Trusted UI Owner only. Metadata is non-secret; copy must be nonblocking and
 * reject a revoked/stale epoch. Neither callback may log/expose credentials,
 * access files/SDK, re-enter UI, or keep the borrowed output pointer. */
typedef struct { bool available; uint64_t epoch; } ryz_v5_password_metadata_t;
typedef struct {
    esp_err_t (*inspect)(void *context, ryz_v5_password_metadata_t *out);
    esp_err_t (*copy)(void *context, uint64_t epoch, char *out, size_t capacity);
    void *context;
} ryz_v5_password_binding_t;

/* Bind before starting the Owner. No secret enters the ordinary status model.
 * Rebinding is allowed only after release/cleanup; NULL disables the reader. */
void ryz_v5_password_bind(const ryz_v5_password_binding_t *binding);
bool ryz_v5_password_refresh(bool eligible);
bool ryz_v5_password_can_hold(void);
bool ryz_v5_password_held(void);
bool ryz_v5_password_begin(void);
/* Wipes the owned text immediately, but physical cleanup is a separate checked
 * barrier. A failed clear must never be interpreted as safe Lua handoff. */
bool ryz_v5_password_revoke(void);
bool ryz_v5_password_needs_cleanup(void);
/* Call only after every system LVGL root has been released. A successful
 * cleanup clears rendered copies; failure retains the mandatory barrier. */
esp_err_t ryz_v5_password_cleanup(void);
/* Original PASS cell only; static text points to bounded private storage until
 * all system roots are released. Never pass its bytes through normal LABEL. */
esp_err_t ryz_v5_password_draw(lv_obj_t *root);
esp_err_t ryz_v5_password_draw_at(lv_obj_t *root, int x, int y, int width);
const char *ryz_v5_password_hint(void);
