#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RYZ_UI_NAV_DEPTH 8

/* Trusted C route identifiers; zero is invalid. No widget/Lua pointers are
 * retained. A token identifies the currently interactive page OR modal, not
 * just a route name. Returning to the same route creates a fresh generation. */
typedef struct {
    uint64_t generation;
    uint32_t route;
    bool modal;
} ryz_ui_nav_token_t;

typedef enum {
    RYZ_UI_NAV_ROOT,
    RYZ_UI_NAV_PUSH,
    RYZ_UI_NAV_REPLACE,
    RYZ_UI_NAV_BACK,
    RYZ_UI_NAV_PRESENT,
    RYZ_UI_NAV_DISMISS,
} ryz_ui_nav_command_t;

typedef enum {
    RYZ_UI_NAV_OK,
    RYZ_UI_NAV_INVALID,
    RYZ_UI_NAV_STALE,
    RYZ_UI_NAV_FULL,
    RYZ_UI_NAV_EMPTY,
    RYZ_UI_NAV_BUSY,
    RYZ_UI_NAV_EXHAUSTED,
} ryz_ui_nav_result_t;

/* Zero-initialize once; thereafter use reset, never memset a live instance.
 * Storage is caller-owned for static embedded allocation. Fields are private
 * implementation state; only this module may read/write them. Single Owner
 * only: cross-task events must copy tokens, not access this state. */
typedef struct {
    uint32_t pages[RYZ_UI_NAV_DEPTH];
    uint32_t modal_route;
    uint64_t generation;
    uint8_t depth;
    bool exhausted;
} ryz_ui_navigation_t;

/* Reset/invalidate revoke every previous token, even on the same route.
 * Generation exhaustion fails closed rather than accepting an ancient token.
 * reset establishes a root and removes modal/history. invalidate keeps them. */
ryz_ui_nav_result_t ryz_ui_nav_reset(ryz_ui_navigation_t *nav, uint32_t root);
ryz_ui_nav_result_t ryz_ui_nav_invalidate(ryz_ui_navigation_t *nav);
ryz_ui_nav_token_t ryz_ui_nav_current(const ryz_ui_navigation_t *nav);
uint32_t ryz_ui_nav_page(const ryz_ui_navigation_t *nav);
bool ryz_ui_nav_matches(const ryz_ui_navigation_t *nav,
                        ryz_ui_nav_token_t token);

/* Stale, invalid, full, empty and busy requests leave state unchanged.
 * ROOT/PUSH/REPLACE/PRESENT require a nonzero route; BACK/DISMISS require 0.
 * One modal may be present. It blocks PUSH/REPLACE/another PRESENT; BACK
 * dismisses it before popping history. ROOT with the active modal token is
 * an explicit reset, whereas underlying-page tokens can never navigate it.
 * BACK at the root and DISMISS without a modal return EMPTY. */
ryz_ui_nav_result_t ryz_ui_nav_apply(ryz_ui_navigation_t *nav,
                                    ryz_ui_nav_token_t expected,
                                    ryz_ui_nav_command_t command,
                                    uint32_t route);

#ifdef __cplusplus
}
#endif
