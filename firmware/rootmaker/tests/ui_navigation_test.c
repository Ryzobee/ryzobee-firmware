#include "ryz_ui_navigation.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test only the public interface: do not inspect or modify navigation fields.
 * In particular, no fabricated counter state is used to claim a tested 64-bit
 * exhaustion boundary. The production pre-increment guard is reviewed
 * statically; executing 2^64 successful transitions is outside this suite. */
static bool equal_token(ryz_ui_nav_token_t a, ryz_ui_nav_token_t b)
{
    return a.generation == b.generation && a.route == b.route && a.modal == b.modal;
}

static ryz_ui_nav_token_t expect_current(const ryz_ui_navigation_t *nav,
                                        uint32_t route, bool modal, uint32_t page)
{
    ryz_ui_nav_token_t token = ryz_ui_nav_current(nav);
    assert(token.generation && token.route == route && token.modal == modal);
    assert(ryz_ui_nav_page(nav) == page);
    assert(ryz_ui_nav_matches(nav, token));
    return token;
}

static ryz_ui_nav_token_t apply(ryz_ui_navigation_t *nav,
                               ryz_ui_nav_command_t command, uint32_t route)
{
    const ryz_ui_nav_token_t before = ryz_ui_nav_current(nav);
    assert(ryz_ui_nav_apply(nav, before, command, route) == RYZ_UI_NAV_OK);
    const ryz_ui_nav_token_t after = ryz_ui_nav_current(nav);
    assert(after.generation > before.generation);
    assert(!ryz_ui_nav_matches(nav, before));
    assert(ryz_ui_nav_matches(nav, after));
    return after;
}

static void reject(ryz_ui_navigation_t *nav, ryz_ui_nav_token_t expected,
                   ryz_ui_nav_command_t command, uint32_t route,
                   ryz_ui_nav_result_t result)
{
    const ryz_ui_nav_token_t before = ryz_ui_nav_current(nav);
    const uint32_t page = ryz_ui_nav_page(nav);
    assert(ryz_ui_nav_apply(nav, expected, command, route) == result);
    assert(equal_token(ryz_ui_nav_current(nav), before));
    assert(ryz_ui_nav_page(nav) == page);
    assert(ryz_ui_nav_matches(nav, before));
}

static void test_initial(void)
{
    ryz_ui_navigation_t nav = {0};
    const ryz_ui_nav_token_t zero = {0};
    assert(equal_token(ryz_ui_nav_current(&nav), zero));
    assert(equal_token(ryz_ui_nav_current(NULL), zero));
    assert(ryz_ui_nav_page(&nav) == 0 && ryz_ui_nav_page(NULL) == 0);
    assert(!ryz_ui_nav_matches(&nav, zero) && !ryz_ui_nav_matches(NULL, zero));
    assert(ryz_ui_nav_reset(NULL, 1) == RYZ_UI_NAV_INVALID);
    assert(ryz_ui_nav_reset(&nav, 0) == RYZ_UI_NAV_INVALID);
    assert(ryz_ui_nav_invalidate(NULL) == RYZ_UI_NAV_INVALID);
    assert(ryz_ui_nav_invalidate(&nav) == RYZ_UI_NAV_INVALID);
    assert(ryz_ui_nav_apply(NULL, zero, RYZ_UI_NAV_ROOT, 1) == RYZ_UI_NAV_INVALID);
    assert(ryz_ui_nav_apply(&nav, zero, RYZ_UI_NAV_ROOT, 1) == RYZ_UI_NAV_STALE);
    assert(equal_token(ryz_ui_nav_current(&nav), zero));
    assert(ryz_ui_nav_reset(&nav, 31) == RYZ_UI_NAV_OK);
    const ryz_ui_nav_token_t current = expect_current(&nav, 31, false, 31);
    ryz_ui_nav_token_t forged = current;
    forged.generation = 0;
    assert(!ryz_ui_nav_matches(&nav, forged));
    forged = current;
    ++forged.generation;
    reject(&nav, forged, RYZ_UI_NAV_ROOT, 1, RYZ_UI_NAV_STALE);
    forged = current;
    ++forged.route;
    reject(&nav, forged, RYZ_UI_NAV_ROOT, 1, RYZ_UI_NAV_STALE);
    forged = current;
    forged.modal = true;
    reject(&nav, forged, RYZ_UI_NAV_ROOT, 1, RYZ_UI_NAV_STALE);
    assert(ryz_ui_nav_reset(&nav, 0) == RYZ_UI_NAV_INVALID);
    assert(equal_token(ryz_ui_nav_current(&nav), current));
}

static void test_stack(void)
{
    ryz_ui_navigation_t nav = {0};
    ryz_ui_nav_token_t history[RYZ_UI_NAV_DEPTH];
    assert(ryz_ui_nav_reset(&nav, 1) == RYZ_UI_NAV_OK);
    history[0] = ryz_ui_nav_current(&nav);
    for (uint32_t i = 1; i < RYZ_UI_NAV_DEPTH; ++i) {
        history[i] = apply(&nav, RYZ_UI_NAV_PUSH, i + 1);
    }
    for (unsigned i = 0; i < 100; ++i) {
        reject(&nav, history[RYZ_UI_NAV_DEPTH - 1], RYZ_UI_NAV_PUSH, 100 + i,
               RYZ_UI_NAV_FULL);
    }
    /* A full page stack still permits the separate, single modal slot. */
    (void)apply(&nav, RYZ_UI_NAV_PRESENT, 90);
    (void)expect_current(&nav, 90, true, RYZ_UI_NAV_DEPTH);
    (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
    for (uint32_t i = RYZ_UI_NAV_DEPTH - 1; i > 0; --i) {
        (void)expect_current(&nav, i + 1, false, i + 1);
        (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
        (void)expect_current(&nav, i, false, i);
        assert(!ryz_ui_nav_matches(&nav, history[i - 1]));
    }
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_BACK, 0, RYZ_UI_NAV_EMPTY);
    /* Full failures have not contaminated future stack slots. */
    for (uint32_t i = 1; i < RYZ_UI_NAV_DEPTH; ++i) {
        (void)apply(&nav, RYZ_UI_NAV_PUSH, 20 + i);
    }
    for (uint32_t i = RYZ_UI_NAV_DEPTH - 1; i > 0; --i) {
        (void)expect_current(&nav, 20 + i, false, 20 + i);
        (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
    }
    (void)expect_current(&nav, 1, false, 1);
}

static void test_same_route(void)
{
    ryz_ui_navigation_t nav = {0};
    assert(ryz_ui_nav_reset(&nav, 7) == RYZ_UI_NAV_OK);
    ryz_ui_nav_token_t first = ryz_ui_nav_current(&nav);
    for (unsigned i = 0; i < 2048; ++i) {
        const ryz_ui_nav_token_t before = ryz_ui_nav_current(&nav);
        (void)apply(&nav, RYZ_UI_NAV_PUSH, 7);
        const ryz_ui_nav_token_t pushed = ryz_ui_nav_current(&nav);
        (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
        (void)expect_current(&nav, 7, false, 7);
        reject(&nav, before, RYZ_UI_NAV_REPLACE, 100, RYZ_UI_NAV_STALE);
        reject(&nav, pushed, RYZ_UI_NAV_BACK, 0, RYZ_UI_NAV_STALE);
        reject(&nav, first, RYZ_UI_NAV_ROOT, 200, RYZ_UI_NAV_STALE);
        (void)apply(&nav, RYZ_UI_NAV_REPLACE, 7);
    }
}

static void test_modal(void)
{
    ryz_ui_navigation_t nav = {0};
    assert(ryz_ui_nav_reset(&nav, 1) == RYZ_UI_NAV_OK);
    const ryz_ui_nav_token_t page = apply(&nav, RYZ_UI_NAV_PUSH, 2);
    const ryz_ui_nav_token_t modal = apply(&nav, RYZ_UI_NAV_PRESENT, 10);
    (void)expect_current(&nav, 10, true, 2);
    reject(&nav, page, RYZ_UI_NAV_ROOT, 3, RYZ_UI_NAV_STALE);
    reject(&nav, page, RYZ_UI_NAV_BACK, 0, RYZ_UI_NAV_STALE);
    reject(&nav, page, RYZ_UI_NAV_DISMISS, 0, RYZ_UI_NAV_STALE);
    reject(&nav, modal, RYZ_UI_NAV_PUSH, 3, RYZ_UI_NAV_BUSY);
    reject(&nav, modal, RYZ_UI_NAV_REPLACE, 3, RYZ_UI_NAV_BUSY);
    reject(&nav, modal, RYZ_UI_NAV_PRESENT, 11, RYZ_UI_NAV_BUSY);
    (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
    (void)expect_current(&nav, 2, false, 2);
    assert(!ryz_ui_nav_matches(&nav, page));
    reject(&nav, modal, RYZ_UI_NAV_ROOT, 3, RYZ_UI_NAV_STALE);
    const ryz_ui_nav_token_t again = apply(&nav, RYZ_UI_NAV_PRESENT, 10);
    assert(again.generation > modal.generation);
    reject(&nav, modal, RYZ_UI_NAV_DISMISS, 0, RYZ_UI_NAV_STALE);
    reject(&nav, modal, RYZ_UI_NAV_ROOT, 4, RYZ_UI_NAV_STALE);
    (void)expect_current(&nav, 10, true, 2);
    (void)apply(&nav, RYZ_UI_NAV_DISMISS, 0);
    (void)expect_current(&nav, 2, false, 2);
    (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
    (void)expect_current(&nav, 1, false, 1);
}

static void test_root_replace(void)
{
    ryz_ui_navigation_t nav = {0};
    assert(ryz_ui_nav_reset(&nav, 1) == RYZ_UI_NAV_OK);
    (void)apply(&nav, RYZ_UI_NAV_PUSH, 2);
    (void)apply(&nav, RYZ_UI_NAV_REPLACE, 3);
    (void)expect_current(&nav, 3, false, 3);
    (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
    (void)expect_current(&nav, 1, false, 1);
    (void)apply(&nav, RYZ_UI_NAV_PUSH, 2);
    (void)apply(&nav, RYZ_UI_NAV_PRESENT, 10);
    (void)apply(&nav, RYZ_UI_NAV_ROOT, 4);
    (void)expect_current(&nav, 4, false, 4);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_BACK, 0, RYZ_UI_NAV_EMPTY);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_DISMISS, 0, RYZ_UI_NAV_EMPTY);
    (void)apply(&nav, RYZ_UI_NAV_REPLACE, 5);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_BACK, 0, RYZ_UI_NAV_EMPTY);
    (void)expect_current(&nav, 5, false, 5);
}

static void test_rejected(void)
{
    ryz_ui_navigation_t nav = {0};
    assert(ryz_ui_nav_reset(&nav, 1) == RYZ_UI_NAV_OK);
    const ryz_ui_nav_command_t require_route[] = {
        RYZ_UI_NAV_ROOT, RYZ_UI_NAV_PUSH, RYZ_UI_NAV_REPLACE, RYZ_UI_NAV_PRESENT,
    };
    for (size_t i = 0; i < sizeof(require_route) / sizeof(require_route[0]); ++i) {
        reject(&nav, ryz_ui_nav_current(&nav), require_route[i], 0, RYZ_UI_NAV_INVALID);
    }
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_BACK, 1, RYZ_UI_NAV_INVALID);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_DISMISS, 1, RYZ_UI_NAV_INVALID);
    reject(&nav, ryz_ui_nav_current(&nav), (ryz_ui_nav_command_t)99, 1,
           RYZ_UI_NAV_INVALID);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_BACK, 0, RYZ_UI_NAV_EMPTY);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_DISMISS, 0, RYZ_UI_NAV_EMPTY);
    const ryz_ui_nav_token_t old = ryz_ui_nav_current(&nav);
    (void)apply(&nav, RYZ_UI_NAV_PUSH, 2);
    reject(&nav, old, RYZ_UI_NAV_REPLACE, 3, RYZ_UI_NAV_STALE);
    (void)apply(&nav, RYZ_UI_NAV_PRESENT, 11);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_PUSH, 3, RYZ_UI_NAV_BUSY);
    (void)apply(&nav, RYZ_UI_NAV_DISMISS, 0);
    (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
    (void)expect_current(&nav, 1, false, 1);
}

static void test_epochs(void)
{
    ryz_ui_navigation_t nav = {0};
    assert(ryz_ui_nav_reset(&nav, 1) == RYZ_UI_NAV_OK);
    (void)apply(&nav, RYZ_UI_NAV_PUSH, 2);
    const ryz_ui_nav_token_t modal = apply(&nav, RYZ_UI_NAV_PRESENT, 2);
    assert(ryz_ui_nav_invalidate(&nav) == RYZ_UI_NAV_OK);
    ryz_ui_nav_token_t current = expect_current(&nav, 2, true, 2);
    assert(current.generation > modal.generation);
    reject(&nav, modal, RYZ_UI_NAV_DISMISS, 0, RYZ_UI_NAV_STALE);
    (void)apply(&nav, RYZ_UI_NAV_DISMISS, 0);
    (void)expect_current(&nav, 2, false, 2);
    (void)apply(&nav, RYZ_UI_NAV_BACK, 0);
    (void)expect_current(&nav, 1, false, 1);
    for (unsigned i = 0; i < 1024; ++i) {
        current = ryz_ui_nav_current(&nav);
        assert(ryz_ui_nav_reset(&nav, 1) == RYZ_UI_NAV_OK);
        assert(ryz_ui_nav_current(&nav).generation > current.generation);
        reject(&nav, current, RYZ_UI_NAV_ROOT, 1, RYZ_UI_NAV_STALE);
        current = ryz_ui_nav_current(&nav);
        assert(ryz_ui_nav_invalidate(&nav) == RYZ_UI_NAV_OK);
        assert(ryz_ui_nav_current(&nav).generation > current.generation);
        reject(&nav, current, RYZ_UI_NAV_ROOT, 1, RYZ_UI_NAV_STALE);
    }
    (void)apply(&nav, RYZ_UI_NAV_PUSH, 2);
    current = apply(&nav, RYZ_UI_NAV_PRESENT, 3);
    assert(ryz_ui_nav_reset(&nav, 1) == RYZ_UI_NAV_OK);
    reject(&nav, current, RYZ_UI_NAV_ROOT, 3, RYZ_UI_NAV_STALE);
    reject(&nav, ryz_ui_nav_current(&nav), RYZ_UI_NAV_BACK, 0, RYZ_UI_NAV_EMPTY);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "initial")) test_initial();
    else if (!strcmp(argv[1], "stack")) test_stack();
    else if (!strcmp(argv[1], "same_route")) test_same_route();
    else if (!strcmp(argv[1], "modal")) test_modal();
    else if (!strcmp(argv[1], "root_replace")) test_root_replace();
    else if (!strcmp(argv[1], "rejected")) test_rejected();
    else if (!strcmp(argv[1], "epochs")) test_epochs();
    else abort();
    printf("UI_NAVIGATION_PASS %s\n", argv[1]);
    return 0;
}
