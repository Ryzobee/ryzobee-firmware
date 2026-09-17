#include "ryz_ui_navigation.h"

#include <stddef.h>
#include <string.h>

static bool active(const ryz_ui_navigation_t *nav)
{
    return nav && !nav->exhausted && nav->depth && nav->generation;
}

static bool advance(ryz_ui_navigation_t *nav)
{
    /* Never increment UINT64_MAX: wrap would make an ancient token usable.
     * Exhaustion is terminal even for reset, and revokes the final token. */
    if (nav->exhausted || nav->generation == UINT64_MAX) {
        nav->exhausted = true;
        return false;
    }
    ++nav->generation;
    return true;
}

ryz_ui_nav_result_t ryz_ui_nav_reset(ryz_ui_navigation_t *nav, uint32_t root)
{
    if (!nav) return RYZ_UI_NAV_INVALID;
    if (nav->exhausted) return RYZ_UI_NAV_EXHAUSTED;
    if (!root) return RYZ_UI_NAV_INVALID;
    if (!advance(nav)) return RYZ_UI_NAV_EXHAUSTED;
    memset(nav->pages, 0, sizeof(nav->pages));
    nav->pages[0] = root;
    nav->depth = 1;
    nav->modal_route = 0;
    return RYZ_UI_NAV_OK;
}

ryz_ui_nav_result_t ryz_ui_nav_invalidate(ryz_ui_navigation_t *nav)
{
    if (!nav) return RYZ_UI_NAV_INVALID;
    if (nav->exhausted) return RYZ_UI_NAV_EXHAUSTED;
    if (!active(nav)) return RYZ_UI_NAV_INVALID;
    return advance(nav) ? RYZ_UI_NAV_OK : RYZ_UI_NAV_EXHAUSTED;
}

ryz_ui_nav_token_t ryz_ui_nav_current(const ryz_ui_navigation_t *nav)
{
    if (!active(nav)) return (ryz_ui_nav_token_t){0};
    return (ryz_ui_nav_token_t){
        .generation = nav->generation,
        .route = nav->modal_route ? nav->modal_route : nav->pages[nav->depth - 1],
        .modal = nav->modal_route != 0,
    };
}

uint32_t ryz_ui_nav_page(const ryz_ui_navigation_t *nav)
{
    return active(nav) ? nav->pages[nav->depth - 1] : 0;
}

bool ryz_ui_nav_matches(const ryz_ui_navigation_t *nav, ryz_ui_nav_token_t token)
{
    if (!active(nav) || !token.generation || !token.route) return false;
    const ryz_ui_nav_token_t current = ryz_ui_nav_current(nav);
    return token.generation == current.generation &&
           token.route == current.route && token.modal == current.modal;
}

ryz_ui_nav_result_t ryz_ui_nav_apply(ryz_ui_navigation_t *nav,
                                    ryz_ui_nav_token_t expected,
                                    ryz_ui_nav_command_t command,
                                    uint32_t route)
{
    if (!nav) return RYZ_UI_NAV_INVALID;
    if (nav->exhausted) return RYZ_UI_NAV_EXHAUSTED;
    switch (command) {
    case RYZ_UI_NAV_ROOT:
    case RYZ_UI_NAV_PUSH:
    case RYZ_UI_NAV_REPLACE:
    case RYZ_UI_NAV_PRESENT:
        if (!route) return RYZ_UI_NAV_INVALID;
        break;
    case RYZ_UI_NAV_BACK:
    case RYZ_UI_NAV_DISMISS:
        if (route) return RYZ_UI_NAV_INVALID;
        break;
    default:
        return RYZ_UI_NAV_INVALID;
    }
    if (!ryz_ui_nav_matches(nav, expected)) return RYZ_UI_NAV_STALE;

    /* Validate every recoverable failure before advancing the generation or
     * touching history. A modal authorizes only its own dismissal or ROOT. */
    if (nav->modal_route && (command == RYZ_UI_NAV_PUSH ||
                            command == RYZ_UI_NAV_REPLACE ||
                            command == RYZ_UI_NAV_PRESENT)) {
        return RYZ_UI_NAV_BUSY;
    }
    if (command == RYZ_UI_NAV_PUSH && nav->depth == RYZ_UI_NAV_DEPTH) {
        return RYZ_UI_NAV_FULL;
    }
    if (command == RYZ_UI_NAV_BACK && !nav->modal_route && nav->depth == 1) {
        return RYZ_UI_NAV_EMPTY;
    }
    if (command == RYZ_UI_NAV_DISMISS && !nav->modal_route) {
        return RYZ_UI_NAV_EMPTY;
    }
    if (command == RYZ_UI_NAV_ROOT) return ryz_ui_nav_reset(nav, route);
    if (!advance(nav)) return RYZ_UI_NAV_EXHAUSTED;

    switch (command) {
    case RYZ_UI_NAV_PUSH:
        nav->pages[nav->depth++] = route;
        break;
    case RYZ_UI_NAV_REPLACE:
        nav->pages[nav->depth - 1] = route;
        break;
    case RYZ_UI_NAV_BACK:
        if (nav->modal_route) nav->modal_route = 0;
        else nav->pages[--nav->depth] = 0;
        break;
    case RYZ_UI_NAV_PRESENT:
        nav->modal_route = route;
        break;
    case RYZ_UI_NAV_DISMISS:
        nav->modal_route = 0;
        break;
    case RYZ_UI_NAV_ROOT:
        break; /* Handled by reset above. */
    }
    return RYZ_UI_NAV_OK;
}
