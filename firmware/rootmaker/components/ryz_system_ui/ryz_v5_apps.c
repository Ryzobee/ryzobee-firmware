#include "ryz_v5_apps.h"
#include "ryz_v5_widgets.h"
#include "ryz_v5_font.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* Original V5: 55:1729 is the APPS stream root; 79:2027 / 79:2074 /
 * 79:2121 / 88:2391 are its Lua browser / hold / detail / delete children.
 * Latest V5 unifies the root, index and hold views into one stream list. */
#define ORANGE 0xff6a00U
#define BACKGROUND 0x121212U
#define SURFACE 0x181818U
#define SELECTED 0x242424U
#define BORDER 0x3a3a3aU
#define WHITE 0xffffffU
#define SECONDARY 0xa0a0a0U
#define DISABLED 0x666666U
#define ERROR_COLOR 0xff453aU
#define ROW_PITCH 29
#define LIST_HEIGHT 135
#define DETAIL_HEIGHT 158
#define DIRECTION_PX 10
#define ROW_FEEDBACK_MS 80U
#define TAP_MS 500U
#define HOLD_MS 2000U
#define MOVE_PX 12

typedef enum { TARGET_NONE, TARGET_ROW, TARGET_RUN, TARGET_DELETE,
               TARGET_CANCEL, TARGET_CONFIRM, TARGET_BACK, TARGET_RETRY,
               TARGET_SCROLL } target_t;

static ryz_v5_apps_binding_t binding;
/* UI Owner task only. Keep distinct old/new snapshots for gesture identity
 * checks; these cache-on bulk copies need not consume internal RAM. */
static EXT_RAM_BSS_ATTR ryz_v5_apps_snapshot_t model, incoming;
/* One bounded reader window, not an unbounded list or file/source cache. */
static EXT_RAM_BSS_ATTR ryz_script_store_page_t list_page;
static ryz_v5_apps_token_t press_token, dialog_token, submitted_delete_token;
static char armed_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
static char press_sha[65];
static char dialog_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U], dialog_sha[65];
static target_t target;
static size_t row;
static uint16_t down_x, down_y, last_y;
static uint64_t down_ms;
static unsigned progress;
static bool pressed, wait_release, moved, triggered, dirty;
static bool row_feedback;
static bool dialog, draw_ok;
static bool await_catalog, catalog_recovery_requested;
static bool run_submitted;
static bool window_pending, window_failed, list_valid, relocating;
static uint64_t window_request_from;
static uint32_t relocate_revision, press_revision;
static size_t relocate_lower;
static int list_scroll, detail_scroll, detail_content_height = 174;
static int anchor_inset;
static char anchor_name[RYZ_SCRIPT_STORE_NAME_MAX + 1U];
static unsigned axis; /* 0 pending, 1 horizontal, 2 vertical: never relock. */
static esp_err_t local_error;
/* Only current-page visual handles are retained. Reader revisions and dialogs
 * invalidate them; business state and operation admission are never cached. */
static uint32_t content_revision;
static struct {
    lv_obj_t *root, *button, *caption, *bar, *fill_caption, *content, *thumb;
    uint32_t revision;
    int scroll;
    esp_err_t error;
    bool styled, holding, runnable;
} feedback;
enum { LIST_ROW_POOL=6 }; /* Five visible rows plus one crossing either edge. */
typedef struct {
    lv_obj_t *root,*content,*thumb;
    lv_obj_t *groups[LIST_ROW_POOL],*rows[LIST_ROW_POOL],*accents[LIST_ROW_POOL];
    lv_obj_t *names[LIST_ROW_POOL],*sizes[LIST_ROW_POOL];
    bool selected[LIST_ROW_POOL];
    size_t indices[LIST_ROW_POOL],count;
    uint32_t revision;
    esp_err_t error;
} list_feedback_t;
static list_feedback_t *list_feedback;

static size_t first_pool_row(void)
{
    int first=list_scroll/ROW_PITCH-(int)list_page.offset;
    if(first<0) first=0;
    if((size_t)first>=list_page.count) first=(int)list_page.count-1;
    return (size_t)first;
}

static void compact_size(char *out,size_t capacity,size_t bytes)
{
    if(bytes>=1024U) snprintf(out,capacity,"%u.%uK",(unsigned)(bytes/1024U),
        (unsigned)(bytes%1024U*10U/1024U));
    else snprintf(out,capacity,"%uB",(unsigned)bytes);
}

static bool selected_row(size_t i)
{
    return pressed && row_feedback && target==TARGET_ROW && !moved ?
        !strcmp(armed_name,list_page.entries[i].name) : list_page.offset+i==0;
}

static void list_deleted(lv_event_t *event)
{
    list_feedback_t *cache=lv_event_get_user_data(event);
    if(list_feedback==cache) list_feedback=NULL;
    lv_free(cache);
}

static void update_list(void)
{
    lv_obj_set_y(list_feedback->content,(int)list_page.offset*ROW_PITCH-list_scroll);
    if(list_feedback->thumb) {
        int height=(int)list_page.total*ROW_PITCH-1;
        unsigned thumb=LIST_HEIGHT*LIST_HEIGHT/(unsigned)height;
        if(thumb<4) thumb=4;
        unsigned top=(LIST_HEIGHT-thumb)*(unsigned)list_scroll/(unsigned)(height-LIST_HEIGHT);
        lv_obj_set_y(list_feedback->thumb,57+(int)top);
    }
    size_t first=first_pool_row();
    for(size_t i=0;i<list_feedback->count;++i) {
        size_t index=first+i;
        if(index>=list_page.count) {
            lv_obj_add_flag(list_feedback->groups[i],LV_OBJ_FLAG_HIDDEN); continue;
        }
        lv_obj_remove_flag(list_feedback->groups[i],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(list_feedback->groups[i],(int)index*ROW_PITCH);
        if(list_feedback->indices[i]!=index) {
            list_feedback->indices[i]=index;
            lv_label_set_text(list_feedback->names[i],list_page.entries[index].name);
            char size[32]; compact_size(size,sizeof(size),list_page.entries[index].bytes);
            lv_label_set_text(list_feedback->sizes[i],size);
        }
        bool selected=selected_row(index);
        if(list_feedback->selected[i]==selected) continue;
        list_feedback->selected[i]=selected;
        lv_obj_set_style_bg_color(list_feedback->rows[i],lv_color_hex(selected?SELECTED:SURFACE),0);
        if(selected) lv_obj_remove_flag(list_feedback->accents[i],LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(list_feedback->accents[i],LV_OBJ_FLAG_HIDDEN);
    }
}

static bool same_page(ryz_v5_apps_token_t a, ryz_v5_apps_token_t b)
{
    return a.page.generation == b.page.generation && a.page.route == b.page.route &&
        a.page.modal == b.page.modal;
}

static bool same_token(ryz_v5_apps_token_t a, ryz_v5_apps_token_t b)
{ return same_page(a, b) && a.reader_request_id == b.reader_request_id; }

static bool catalog(void)
{ return model.reader.view != RYZ_APPS_VIEW_DETAIL; }

static bool ready_detail(void)
{
    return model.open && model.reader.view == RYZ_APPS_VIEW_DETAIL &&
        model.reader.state == RYZ_APPS_READY && model.reader.store_ready &&
        !model.reader.recovery_required;
}

static bool idle_operation(void)
{
    return (model.run_state == RYZ_V5_APPS_IDLE || model.run_state == RYZ_V5_APPS_FAILED) &&
        model.delete_state != RYZ_V5_APPS_DELETE_PENDING &&
        !(model.delete_state == RYZ_V5_APPS_DELETE_UNKNOWN &&
          same_token(submitted_delete_token, model.token));
}

static void clear_press(void)
{
    pressed = false;
    target = TARGET_NONE;
    moved = false;
    triggered = false;
    progress = 0;
    axis = 0;
    row_feedback = false;
}

void ryz_v5_apps_cancel_gesture(void)
{
    clear_press();
    wait_release = true;
    dirty = true;
}

void ryz_v5_apps_reset(void)
{
    ++content_revision;
    memset(&model, 0, sizeof(model));
    memset(&incoming, 0, sizeof(incoming));
    clear_press();
    await_catalog = true;
    catalog_recovery_requested = false;
    dialog = false;
    submitted_delete_token = (ryz_v5_apps_token_t){0};
    run_submitted = false;
    wait_release = true;
    window_pending = false;
    window_failed = false;
    relocating = false;
    detail_scroll = 0;
    local_error = ESP_OK;
    dirty = true;
}

void ryz_v5_apps_bind(const ryz_v5_apps_binding_t *value)
{
    binding = value ? *value : (ryz_v5_apps_binding_t){0};
    memset(&list_page, 0, sizeof(list_page));
    list_valid = false;
    list_scroll = anchor_inset = 0;
    anchor_name[0] = 0;
    ryz_v5_apps_reset();
}

static bool submit(ryz_v5_apps_action_t action, ryz_v5_apps_token_t token,
                   size_t offset, size_t limit, size_t index)
{
    ryz_v5_apps_intent_t intent = {
        .expected = token, .action = action, .offset = offset,
        .limit = limit, .index = index,
    };
    if (action == RYZ_V5_APPS_DELETE) {
        snprintf(intent.delete_name, sizeof(intent.delete_name), "%s", dialog_name);
        snprintf(intent.delete_sha256, sizeof(intent.delete_sha256), "%s", dialog_sha);
    }
    local_error = binding.submit ? binding.submit(binding.context, &intent)
                                : ESP_ERR_NOT_SUPPORTED;
    if (action == RYZ_V5_APPS_DELETE && local_error == ESP_OK) submitted_delete_token = token;
    dirty = true;
    return local_error == ESP_OK;
}

static bool dialog_current(void)
{
    return ready_detail() && same_token(dialog_token, model.token) &&
        !strcmp(dialog_name, model.reader.detail.file.entry.name) &&
        !strcmp(dialog_sha, model.reader.detail.file.sha256);
}

static int clamp_scroll(int64_t offset, int content, int viewport)
{
    int maximum = content > viewport ? content - viewport : 0;
    return offset < 0 ? 0 : offset > maximum ? maximum : (int)offset;
}

static void remember_anchor(void)
{
    if (!list_valid || relocating || !list_page.count) return;
    size_t first = (size_t)(list_scroll / ROW_PITCH);
    if (first < list_page.offset || first >= list_page.offset + list_page.count) return;
    snprintf(anchor_name, sizeof(anchor_name), "%s",
             list_page.entries[first - list_page.offset].name);
    anchor_inset = list_scroll % ROW_PITCH;
}

static bool request_window(size_t offset)
{
    if (window_pending || window_failed || !model.open || !catalog()) return false;
    if (!submit(RYZ_V5_APPS_PAGE, model.token, offset, RYZ_SCRIPT_STORE_PAGE_MAX, 0)) {
        window_failed = true;
        return false;
    }
    window_pending = true;
    window_request_from = model.token.reader_request_id;
    return true;
}

static void ensure_window(void)
{
    if (!list_valid || relocating || window_pending || window_failed || !model.open || !catalog() ||
        (model.reader.state != RYZ_APPS_READY && model.reader.state != RYZ_APPS_EMPTY)) return;
    size_t first = (size_t)(list_scroll / ROW_PITCH);
    size_t end = (size_t)((list_scroll + LIST_HEIGHT + ROW_PITCH - 1) / ROW_PITCH);
    if (end > list_page.total) end = list_page.total;
    /* Keep four leading rows of overlap for reverse drags. At most sixteen
     * records are requested; the latest desired pixel offset stays local. */
    if (model.page_limit != RYZ_SCRIPT_STORE_PAGE_MAX || first < list_page.offset ||
        end > list_page.offset + list_page.count) {
        size_t offset = first > 4U ? first - 4U : 0U;
        size_t maximum = list_page.total > RYZ_SCRIPT_STORE_PAGE_MAX ?
            list_page.total - RYZ_SCRIPT_STORE_PAGE_MAX : 0U;
        if (offset > maximum) offset = maximum;
        (void)request_window(offset);
    }
}

static void accept_catalog(void)
{
    if (!catalog() || (model.reader.state != RYZ_APPS_READY && model.reader.state != RYZ_APPS_EMPTY)) return;
    if (window_pending && window_request_from == model.token.reader_request_id) return;
    window_pending = false;
    const ryz_script_store_page_t *page = &model.reader.page;
    if (page->count > RYZ_SCRIPT_STORE_PAGE_MAX || page->total > RYZ_SCRIPT_STORE_INDEX_MAX ||
        page->offset > page->total || page->count > page->total - page->offset) {
        local_error = ESP_ERR_INVALID_STATE;
        list_valid = false;
        return;
    }
    if (list_valid && page->revision != list_page.revision && anchor_name[0] && !relocating) {
        relocating = true;
        relocate_revision = page->revision;
        relocate_lower = 0;
        ryz_v5_apps_cancel_gesture();
    }
    list_page = *page;
    list_valid = true;
    list_scroll = clamp_scroll(list_scroll, (int)page->total * ROW_PITCH - 1, LIST_HEIGHT);
    if (relocating) {
        if (page->revision != relocate_revision) {
            /* A second directory change cancels restoration. Do not chase a
             * moving target or revive the old contact across revisions. */
            relocating = false;
            anchor_name[0] = 0;
            local_error = ESP_ERR_INVALID_STATE;
        } else {
            size_t index = page->count;
            for (size_t i = 0; i < page->count; ++i) {
                if (strcmp(page->entries[i].name, anchor_name) >= 0) { index = i; break; }
            }
            if (index < page->count &&
                (!strcmp(page->entries[index].name, anchor_name) ||
                 page->offset <= relocate_lower || index > 0)) {
                list_scroll = clamp_scroll((int)(page->offset + index) * ROW_PITCH + anchor_inset,
                                           (int)page->total * ROW_PITCH - 1, LIST_HEIGHT);
                relocating = false;
            } else if (page->count && strcmp(page->entries[0].name, anchor_name) > 0 && page->offset) {
                (void)request_window(page->offset > RYZ_SCRIPT_STORE_PAGE_MAX ?
                                     page->offset - RYZ_SCRIPT_STORE_PAGE_MAX : 0U);
            } else if (page->offset + page->count < page->total) {
                /* The next page's first item is the nearest successor when
                 * the anchor was deleted exactly at a window boundary. */
                relocate_lower = page->offset + page->count;
                (void)request_window(page->offset + page->count);
            } else {
                /* Removed final anchor: nearest surviving row, clamped. */
                relocating = false;
            }
        }
    }
    if (!relocating) { remember_anchor(); ensure_window(); }
}

void ryz_v5_apps_scroll(int delta_y)
{
    ryz_v5_apps_cancel_gesture();
    if (dialog || await_catalog) return;
    if (catalog()) {
        if (!list_valid || relocating) return;
        window_failed = false;
        list_scroll = clamp_scroll((int64_t)list_scroll + delta_y,
                                  (int)list_page.total * ROW_PITCH - 1, LIST_HEIGHT);
        remember_anchor();
        ensure_window();
    } else if (ready_detail()) {
        detail_scroll = clamp_scroll((int64_t)detail_scroll + delta_y, detail_content_height, DETAIL_HEIGHT);
    }
}

bool ryz_v5_apps_tick(uint64_t now_ms)
{
    esp_err_t error = binding.get ? binding.get(binding.context, &incoming)
                                  : ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK) {
        bool changed = memcmp(&model, &incoming, sizeof(model)) != 0;
        if (changed) {
            ++content_revision;
            bool gesture_identity_changed = target == TARGET_BACK ?
                !same_page(model.token, incoming.token) :
                !same_token(model.token, incoming.token);
            bool same_scrolling_directory = pressed && moved && axis == 2 && catalog() &&
                incoming.reader.view == RYZ_APPS_VIEW_CATALOG &&
                same_page(model.token, incoming.token) &&
                (incoming.reader.state == RYZ_APPS_LOADING ||
                 incoming.reader.page.revision == list_page.revision);
            if (incoming.reader.view == RYZ_APPS_VIEW_DETAIL &&
                (model.reader.view != RYZ_APPS_VIEW_DETAIL ||
                 strcmp(model.reader.detail.file.entry.name, incoming.reader.detail.file.entry.name))) {
                detail_scroll = 0;
                run_submitted = false;
            }
            if (incoming.run_state != RYZ_V5_APPS_IDLE) run_submitted = false;
            model = incoming;
            if (gesture_identity_changed && pressed && !triggered && !same_scrolling_directory) {
                clear_press();
                wait_release = true;
            }
            if (pressed && target == TARGET_RUN && !triggered &&
                (!ready_detail() || !idle_operation() || !model.reader.detail.source_valid ||
                 strcmp(armed_name, model.reader.detail.file.entry.name) ||
                 strcmp(press_sha, model.reader.detail.file.sha256) ||
                 press_revision != model.reader.detail.revision))
                ryz_v5_apps_cancel_gesture();
            if (dialog && !dialog_current()) {
                dialog = false;
                local_error = ESP_ERR_INVALID_STATE;
                clear_press();
                wait_release = true;
            }
            dirty = true;
        }
        if (await_catalog && catalog()) await_catalog = false;
        /* Route re-entry discards late detail/hold results. Request one fresh
         * catalog and wait for its acknowledgement; never replay an action. */
        if (await_catalog && model.open && !catalog() && !catalog_recovery_requested) {
            catalog_recovery_requested = true;
            (void)submit(RYZ_V5_APPS_BACK, model.token, 0, 0, 0);
        }
        if (model.open) accept_catalog();
        if (window_pending && model.token.reader_request_id != window_request_from &&
            model.reader.state != RYZ_APPS_LOADING && model.reader.state != RYZ_APPS_IDLE)
            window_pending = false;
    } else if (error != ESP_ERR_TIMEOUT && local_error != error) {
        ++content_revision;
        local_error = error;
        memset(&model, 0, sizeof(model));
        model.reader.state = RYZ_APPS_FAILED;
        model.reader.error = error;
        clear_press();
        dialog = false;
        window_pending = false;
        dirty = true;
    }
    /* Keep the initial list contact cheap: the same Owner polls TP and
     * renders LVGL synchronously. Give a swipe time to acquire movement
     * before rebuilding the page merely to highlight a pressed row. */
    if (pressed && !moved && target == TARGET_ROW && !row_feedback &&
        now_ms >= down_ms && now_ms - down_ms >= ROW_FEEDBACK_MS) {
        row_feedback = true;
        dirty = true;
    }
    if (pressed && !moved && !triggered && target == TARGET_RUN) {
        uint64_t elapsed = now_ms >= down_ms ? now_ms - down_ms : 0;
        unsigned next = elapsed >= HOLD_MS ? 100U : (unsigned)(elapsed * 100U / HOLD_MS);
        if (next != progress) { progress = next; dirty = true; }
        if (elapsed >= HOLD_MS) {
            triggered = true;
            if (!same_token(press_token, model.token)) local_error = ESP_ERR_INVALID_STATE;
            else if (ready_detail() && model.reader.detail.source_valid && idle_operation() &&
                       !strcmp(armed_name, model.reader.detail.file.entry.name) &&
                       press_revision == model.reader.detail.revision &&
                       !strcmp(press_sha, model.reader.detail.file.sha256))
                run_submitted = submit(RYZ_V5_APPS_RUN, press_token, 0, 0, 0);
            else local_error = ESP_ERR_INVALID_STATE;
            dirty = true;
        }
    }
    bool result = dirty;
    dirty = false;
    return result;
}

static bool within(uint16_t x, uint16_t y, int left, int top, int width, int height)
{ return x >= left && y >= top && x < left + width && y < top + height; }

static target_t hit(uint16_t x, uint16_t y)
{
    if (within(x, y, 0, 0, 44, 32))
        return !dialog && (catalog() || await_catalog) ? TARGET_NONE : TARGET_BACK;
    if (y < 32) return TARGET_NONE;
    if (dialog) {
        if (within(x, y, 24, 190, 88, 30)) return TARGET_CANCEL;
        if (within(x, y, 120, 190, 96, 30)) return TARGET_CONFIRM;
        return TARGET_SCROLL; /* Modal consumes every background sample. */
    }
    if (!catalog() && !await_catalog) {
        if (within(x, y, 80, 196, 152, 44)) return TARGET_RUN;
        if (within(x, y, 8, 196, 68, 44)) return TARGET_DELETE;
        return TARGET_SCROLL;
    }
    if (y >= 196) return TARGET_NONE;
    if (await_catalog ||
        (model.reader.state != RYZ_APPS_READY && model.reader.state != RYZ_APPS_EMPTY))
        return within(x, y, 8, 132, 224, 40) ? TARGET_RETRY : TARGET_SCROLL;
    if (!list_valid || window_pending || relocating) return TARGET_SCROLL;
    if (within(x, y, 8, 57, 224, LIST_HEIGHT)) {
        size_t index = (size_t)(((int)y - 57 + list_scroll) / ROW_PITCH);
        int inside = ((int)y - 57 + list_scroll) % ROW_PITCH;
        if (inside < 28 && index >= model.reader.page.offset &&
            index < model.reader.page.offset + model.reader.page.count) {
            row = index - model.reader.page.offset;
            return TARGET_ROW;
        }
    }
    return TARGET_SCROLL;
}

bool ryz_v5_apps_pointer(const ryz_touch_sample_t *sample, uint64_t now_ms)
{
    if (!sample) { ryz_v5_apps_cancel_gesture(); return true; }
    if (sample->pressed && sample->has_position &&
        (sample->x >= RYZ_TOUCH_WIDTH || sample->y >= RYZ_TOUCH_HEIGHT)) {
        clear_press(); wait_release = true; dirty = true; return true;
    }
    if (wait_release) {
        if (!sample->pressed) wait_release = false;
        return true;
    }
    if (sample->event == RYZ_TOUCH_NONE && !sample->pressed) {
        /* TP can observe release without producing an UP edge. This ends
         * ownership, but never commits the captured row or action. A later
         * UP is inert; the next physical DOWN can begin a fresh contact. */
        const bool captured = pressed;
        clear_press();
        if (captured) dirty = true;
        return captured;
    }
    if (pressed && sample->event == RYZ_TOUCH_DOWN) {
        /* A duplicated DOWN cannot replace or extend an in-flight capture,
         * including a RUN hold. Require release before accepting new input. */
        ryz_v5_apps_cancel_gesture();
        return true;
    }
    if (sample->pressed && !pressed) {
        /* MOVE from the shell navigation is not a new list contact. */
        if (sample->event != RYZ_TOUCH_DOWN) return false;
        if (!sample->has_position) return false;
        target = hit(sample->x, sample->y);
        if (target == TARGET_NONE) return false;
        down_x = sample->x; down_y = last_y = sample->y; down_ms = now_ms;
        pressed = true; moved = false; triggered = false; progress = 0;
        axis = 0;
        row_feedback = false;
        press_token = model.token;
        if (target == TARGET_ROW) {
            press_revision = model.reader.page.revision;
            snprintf(armed_name, sizeof(armed_name), "%s", model.reader.page.entries[row].name);
        } else if (target == TARGET_RUN) {
            snprintf(press_sha, sizeof(press_sha), "%s", model.reader.detail.file.sha256);
            snprintf(armed_name, sizeof(armed_name), "%s", model.reader.detail.file.entry.name);
            press_revision = model.reader.detail.revision;
            if (!ready_detail() || !model.reader.detail.source_valid || !idle_operation() || run_submitted) moved = true;
        }
        if (target != TARGET_ROW || local_error != ESP_OK) dirty = true;
        local_error = ESP_OK;
        return true;
    }
    if (!pressed) return false;
    /* CST816 gesture 0x01..0x04 means a slide. Fast polling contacts can
     * contain only DOWN and a positionless UP when intermediate coordinates
     * were missed. Such a release must never become a selection, Back or
     * destructive confirmation in a detail/modal page either.
     * Use this only as a veto, not a direction: the raw gesture is not
     * rotated with display coordinates. Ignore stale gesture bytes on DOWN. */
    if (sample->event == RYZ_TOUCH_UP &&
        sample->gesture >= 0x01 && sample->gesture <= 0x04) moved = true;
    if (!sample->pressed && sample->has_position) {
        /* Some input backends only report their final displacement on UP.
         * Apply that sample to the same capture before deciding a tap. */
        ryz_touch_sample_t final_move = *sample;
        final_move.pressed = true;
        final_move.event = RYZ_TOUCH_MOVE;
        (void)ryz_v5_apps_pointer(&final_move, now_ms);
        if (!pressed) { wait_release = false; return true; }
    }
    if (sample->pressed && sample->has_position) {
        int dx = (int)sample->x - down_x, dy = (int)sample->y - down_y;
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        bool outside = (target == TARGET_ROW &&
            !within(sample->x, sample->y, 8,
                57 + (int)(model.reader.page.offset + row) * ROW_PITCH - list_scroll, 224, 28)) ||
            (target == TARGET_RUN && !within(sample->x, sample->y, 80, 196, 152, 44)) ||
            (target == TARGET_DELETE && !within(sample->x, sample->y, 8, 196, 68, 44)) ||
            (target == TARGET_CANCEL && !within(sample->x, sample->y, 24, 190, 88, 30)) ||
            (target == TARGET_CONFIRM && !within(sample->x, sample->y, 120, 190, 96, 30)) ||
            (target == TARGET_BACK &&
                !within(sample->x, sample->y, 0, 0, 44, 32));
        /* The back affordance is a small target, so a physical touch can
         * move more than MOVE_PX while it is still visibly held on the
         * arrow.  List rows and action buttons retain the distance guard;
         * back is cancelled only after the contact leaves its hit box. */
        const bool distance_cancel = target != TARGET_BACK &&
            (dx * dx + dy * dy >= DIRECTION_PX * DIRECTION_PX ||
             dx * dx + dy * dy > MOVE_PX * MOVE_PX);
        if (!moved && (outside || distance_cancel)) {
            moved = true; progress = 0; dirty = true;
        }
        unsigned before = axis;
        if (!axis && (ax >= DIRECTION_PX || ay >= DIRECTION_PX)) {
            if (ax * 2 >= ay * 3) axis = 1;
            else if (ay * 2 >= ax * 3) axis = 2;
        }
        bool list_drag = catalog() && down_y >= 57 && down_y < 57 + LIST_HEIGHT;
        bool detail_drag = !catalog() && down_y >= 34 && down_y < 34 + DETAIL_HEIGHT;
        if (axis == 2 && !triggered && !dialog && (list_drag || detail_drag)) {
            int delta = (before == 2 ? (int)last_y : (int)down_y) - sample->y;
            if (list_drag && list_valid && !relocating) {
                window_failed = false;
                list_scroll = clamp_scroll(list_scroll + delta,
                    (int)list_page.total * ROW_PITCH - 1, LIST_HEIGHT);
                remember_anchor(); ensure_window();
            } else if (detail_drag && ready_detail())
                detail_scroll = clamp_scroll(detail_scroll + delta, detail_content_height, DETAIL_HEIGHT);
            dirty = true;
        }
        last_y = sample->y;
        return true;
    }
    if (sample->pressed) { ryz_v5_apps_cancel_gesture(); return true; }
    /* A positionless UP is valid. Movement permanently cancels this capture. */
    const bool press_current = target == TARGET_BACK ?
        same_page(press_token, model.token) : same_token(press_token, model.token);
    if (!moved && !triggered && press_current) {
        if (target == TARGET_ROW && now_ms >= down_ms && now_ms - down_ms < TAP_MS &&
            !window_pending && !relocating && row < model.reader.page.count &&
            model.reader.page.revision == press_revision &&
            !strcmp(armed_name, model.reader.page.entries[row].name))
            (void)submit(RYZ_V5_APPS_SELECT, press_token, 0, 0, row);
        else if (target == TARGET_BACK || target == TARGET_CANCEL) {
            if (dialog) dialog = false;
            else if (!catalog() && !await_catalog &&
                     submit(RYZ_V5_APPS_BACK, model.token, 0, 0, 0)) {
                await_catalog = true;
                catalog_recovery_requested = true;
            }
        } else if (target == TARGET_DELETE && ready_detail() && idle_operation()) {
            if (model.reader.detail.file.entry.protected_file) local_error = ESP_ERR_NOT_SUPPORTED;
            else {
                dialog = true; dialog_token = model.token;
                snprintf(dialog_name, sizeof(dialog_name), "%s", model.reader.detail.file.entry.name);
                snprintf(dialog_sha, sizeof(dialog_sha), "%s", model.reader.detail.file.sha256);
            }
        } else if (target == TARGET_CONFIRM && dialog_current() && idle_operation()) {
            if (submit(RYZ_V5_APPS_DELETE, dialog_token, 0, 0, 0)) dialog = false;
        } else if (target == TARGET_RETRY) {
            window_failed = false;
            (void)request_window(0);
        }
    }
    clear_press();
    dirty = true;
    return true;
}

const char *ryz_v5_apps_title(void)
{
    if (await_catalog) return "APPS";
    if (dialog) return "DELETE APP";
    return !catalog() ? "APP INFO" : "APPS";
}

bool ryz_v5_apps_has_navigation(void)
{ return !dialog && (catalog() || await_catalog); }

bool ryz_v5_apps_has_back(void)
{ return dialog || (!catalog() && !await_catalog); }

static lv_obj_t *label(lv_obj_t *root, int x, int y, int w, int h, const char *text,
                        ryz_font_face_t face, unsigned px, uint32_t color,
                        lv_text_align_t align)
{
    lv_obj_t *object = ryz_v5_label(root, x, y, w, h, text, face, px, color, align);
    if (!object) draw_ok = false;
    else lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

static void box(lv_obj_t *root, int x, int y, int w, int h,
                 uint32_t bg, uint32_t border, unsigned width)
{
    if (!ryz_v5_box(root, x, y, w, h, bg, border, width)) draw_ok = false;
}

static void rounded_box(lv_obj_t *root, int x, int y, int w, int h,
                         uint32_t bg, uint32_t border, unsigned width, unsigned radius)
{
    lv_obj_t *object = ryz_v5_box(root, x, y, w, h, bg, border, (int)width);
    if (!object) draw_ok = false;
    else lv_obj_set_style_radius(object, (int)radius, 0);
}

static void paragraph(lv_obj_t *root, int x, int y, int w, int h,
                       const char *text, unsigned px, unsigned line_height,
                       uint32_t color)
{
    lv_obj_t *object = label(root, x, y, w, h, text, RYZ_FONT_BODY, px, color,
                              LV_TEXT_ALIGN_LEFT);
    if (!object) return;
    const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    lv_obj_set_style_text_line_space(object, (int)line_height - (int)font->line_height, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
}

static void ascii(char *out, size_t capacity, const char *source)
{
    if (!source || !source[0]) { snprintf(out, capacity, "-"); return; }
    for (const unsigned char *p = (const unsigned char *)source; *p; ++p) {
        if (*p < 32 || *p > 126) { snprintf(out, capacity, "[NON-ASCII]"); return; }
    }
    snprintf(out, capacity, "%s", source);
}

static void size_text(char *out, size_t capacity, size_t bytes)
{
    if (bytes < 1024U) snprintf(out, capacity, "%u B", (unsigned)bytes);
    else snprintf(out, capacity, "%u.%u KB", (unsigned)(bytes / 1024U),
                  (unsigned)((bytes % 1024U) * 10U / 1024U));
}

static const char *problem(void)
{
    if (model.delete_state == RYZ_V5_APPS_DELETE_UNKNOWN) return "DELETE UNKNOWN - DO NOT RETRY";
    if (model.delete_state == RYZ_V5_APPS_DELETE_PENDING) return "DELETING SELECTED FILE";
    if (model.delete_recovery_required) return "DELETE REQUIRES STORAGE RECOVERY";
    if (model.delete_state == RYZ_V5_APPS_DELETE_FAILED) return "DELETE FAILED - RESELECT";
    if (model.run_state == RYZ_V5_APPS_UNKNOWN) return "OUTCOME UNKNOWN - DO NOT RETRY";
    if (model.run_state == RYZ_V5_APPS_PREPARING) return "PREPARING SELECTED FILE";
    if (model.reader.recovery_required) return "STORAGE RECOVERY REQUIRED";
    if (local_error != ESP_OK) return local_error == ESP_ERR_TIMEOUT ? "BUSY - TRY AGAIN" :
        local_error == ESP_ERR_NOT_SUPPORTED ? "OPERATION NOT AVAILABLE" : "VIEW CHANGED - SELECT AGAIN";
    if (model.action_error != ESP_OK) return "ACTION FAILED - SELECT AGAIN";
    if (model.reader.state == RYZ_APPS_STALE) return "FILES CHANGED - RELOAD";
    if (model.reader.state == RYZ_APPS_FAILED) return "CANNOT READ APPLICATIONS";
    return NULL;
}

static void draw_list(lv_obj_t *root)
{
    char text[64];
    const ryz_script_store_page_t *page = &list_page;
    label(root, 8, 34, 100, 18, "LUA FILES",
          RYZ_FONT_MONO_MEDIUM, 12, ORANGE, LV_TEXT_ALIGN_LEFT);
    snprintf(text, sizeof(text), "%02u TOTAL", (unsigned)page->total);
    label(root, 150, 34, 74, 18, text, RYZ_FONT_MONO, 12, SECONDARY, LV_TEXT_ALIGN_RIGHT);
    box(root, 8, 53, 224, 1, BORDER, BORDER, 0);
    bool ready = !await_catalog && !relocating && list_valid &&
        (model.reader.state == RYZ_APPS_READY || model.reader.state == RYZ_APPS_EMPTY ||
         (window_pending && model.reader.state == RYZ_APPS_LOADING));
    if (!ready || !page->count) {
        const char *status = problem();
        if (!status) status = model.reader.state == RYZ_APPS_EMPTY ? "NO LUA FILES" : "LOADING APPLICATIONS";
        label(root, 12, 80, 216, 40, status, RYZ_FONT_BODY, 12, SECONDARY, LV_TEXT_ALIGN_CENTER);
        if (model.reader.state == RYZ_APPS_FAILED || model.reader.state == RYZ_APPS_STALE || local_error != ESP_OK) {
            box(root, 8, 132, 224, 40, SURFACE, ORANGE, 1);
            label(root, 8, 132, 224, 40, "RETRY", RYZ_FONT_BODY_SEMIBOLD, 14, ORANGE, LV_TEXT_ALIGN_CENTER);
        }
        return;
    }
    lv_obj_t *viewport = ryz_v5_box(root, 8, 57, 224, LIST_HEIGHT, BACKGROUND, 0, 0);
    if (!viewport) { draw_ok = false; return; }
    list_feedback_t *cache=lv_malloc(sizeof(*cache));
    if(!cache) { draw_ok=false; return; }
    memset(cache,0,sizeof(*cache));
    cache->root=root; cache->revision=content_revision; cache->error=local_error;
    if(!lv_obj_add_event_cb(root,list_deleted,LV_EVENT_DELETE,cache)) {
        lv_free(cache); draw_ok=false; return;
    }
    list_feedback=cache;
    /* Recycle six visual rows within the bounded reader window. Keeping all
     * 16 rows would exceed the 64 KiB pool during old/new candidate overlap. */
    lv_obj_t *content=ryz_v5_box(viewport,0,(int)page->offset*ROW_PITCH-list_scroll,
        224,(int)page->count*ROW_PITCH,BACKGROUND,0,0);
    if(!content) { draw_ok=false; return; }
    cache->content=content;
    cache->count=page->count<LIST_ROW_POOL?page->count:LIST_ROW_POOL;
    size_t first=first_pool_row();
    for (size_t i = 0; i < cache->count; ++i) {
        size_t index=first+i<page->count?first+i:page->count-1;
        cache->indices[i]=index;
        lv_obj_t *group=ryz_v5_box(content,0,(int)index*ROW_PITCH,224,28,BACKGROUND,0,0);
        if(!group) { draw_ok=false; return; }
        cache->groups[i]=group;
        if(first+i>=page->count) lv_obj_add_flag(group,LV_OBJ_FLAG_HIDDEN);
        bool selected=selected_row(index);
        cache->selected[i]=selected;
        cache->rows[i]=ryz_v5_box(group,0,0,224,28,selected?SELECTED:SURFACE,BORDER,1);
        cache->accents[i]=ryz_v5_box(group,0,0,3,28,ORANGE,ORANGE,0);
        if(!cache->rows[i] || !cache->accents[i]) { draw_ok=false; return; }
        if(!selected) lv_obj_add_flag(cache->accents[i],LV_OBJ_FLAG_HIDDEN);
        label(group, 7, 5, 22, 18, "LUA", RYZ_FONT_MONO_MEDIUM, 12, ORANGE, LV_TEXT_ALIGN_CENTER);
        cache->names[i]=label(group, 34, 4, 106, 20, page->entries[index].name,
              RYZ_FONT_BODY_SEMIBOLD, 12, WHITE, LV_TEXT_ALIGN_LEFT);
        /* Compact units in the fixed 40px column, without shrinking text. */
        compact_size(text,sizeof(text),page->entries[index].bytes);
        cache->sizes[i]=label(group, 142, 5, 40, 18, text, RYZ_FONT_MONO, 12, SECONDARY, LV_TEXT_ALIGN_RIGHT);
        /* Catalog has no parsed metadata. Never invent a version from Figma's
         * sample data or perform file reads while drawing a row. */
        label(group, 184, 5, 34, 18, "-", RYZ_FONT_MONO_MEDIUM, 12, ORANGE, LV_TEXT_ALIGN_RIGHT);
    }
    int content_height = (int)page->total * ROW_PITCH - 1;
    if (content_height > LIST_HEIGHT) {
        box(root, 234, 57, 2, LIST_HEIGHT, BORDER, BORDER, 0);
        unsigned height = LIST_HEIGHT * LIST_HEIGHT / (unsigned)content_height;
        if (height < 4) height = 4;
        unsigned top = (LIST_HEIGHT - height) * (unsigned)list_scroll / (unsigned)(content_height - LIST_HEIGHT);
        cache->thumb=ryz_v5_box(root,234,57+(int)top,2,(int)height,ORANGE,ORANGE,0);
        if(!cache->thumb) draw_ok=false;
    }
    const char *status = problem();
    if (status) {
        box(root, 8, 156, 224, 36, BACKGROUND, ERROR_COLOR, 1);
        paragraph(root, 12, 158, 216, 32, status, 12, 16, ERROR_COLOR);
    }
}

static void cell(lv_obj_t *root, int x, int y, const char *title, const char *value, uint32_t color)
{
    box(root, x, y, 70, 30, SURFACE, BORDER, 1);
    label(root, x + 4, y, 62, 14, title, RYZ_FONT_MONO, 12, SECONDARY, LV_TEXT_ALIGN_LEFT);
    label(root, x + 4, y + 14, 62, 14, value, RYZ_FONT_MONO_MEDIUM, 12, color, LV_TEXT_ALIGN_LEFT);
}

static void display_version(char *out, size_t capacity, const char *raw)
{
    if (!out || !capacity) return;
    if (!raw || !raw[0]) {
        snprintf(out, capacity, "--");
        return;
    }
    /* Bound the copied version text explicitly.  Metadata is user-provided
     * and may be longer than the fixed UI buffer; truncation is intentional. */
    const int text_limit = capacity > 1U && capacity - 1U <= (size_t)INT_MAX
                               ? (int)(capacity - 1U) : INT_MAX;
    const int prefixed_limit = capacity > 2U && capacity - 2U <= (size_t)INT_MAX
                                   ? (int)(capacity - 2U) : INT_MAX;
    if (raw[0] == 'V') snprintf(out, capacity, "%.*s", text_limit, raw);
    else if (raw[0] == 'v') snprintf(out, capacity, "V%.*s", prefixed_limit, raw + 1);
    else if (raw[0] >= '0' && raw[0] <= '9') snprintf(out, capacity, "V%.*s", prefixed_limit, raw);
    else snprintf(out, capacity, "%.*s", text_limit, raw);
}

static void date_text(char out[6], int64_t seconds)
{
    strcpy(out, "-");
    struct tm utc;
    const time_t epoch = (time_t)seconds;
    if (seconds >= INT64_C(1704067200) && seconds <= INT64_C(253402300799) &&
        (int64_t)epoch == seconds && gmtime_r(&epoch, &utc) != NULL) {
        if (strftime(out, 6, "%m-%d", &utc) != 5) strcpy(out, "-");
    }
}

static void feedback_deleted(lv_event_t *event)
{
    if(lv_event_get_target_obj(event)==feedback.root) memset(&feedback,0,sizeof(feedback));
}

static void update_feedback(void)
{
    bool runnable=ready_detail() && model.reader.detail.source_valid && idle_operation() && !run_submitted;
    bool holding=pressed && target==TARGET_RUN && !moved && !triggered;
    const uint32_t bg=holding?0:runnable?ORANGE:SELECTED;
    const uint32_t color=holding?ORANGE:runnable?0:SECONDARY;
    /* LVGL style setters invalidate even equal values. Only change styles
     * at state edges; progress updates remain confined to the button. */
    if(!feedback.styled || feedback.holding!=holding || feedback.runnable!=runnable) {
        lv_obj_set_style_bg_color(feedback.button,lv_color_hex(bg),0);
        lv_obj_set_style_border_color(feedback.button,lv_color_hex(holding?ORANGE:BORDER),0);
        lv_obj_set_style_border_width(feedback.button,holding?1:0,0);
        lv_obj_set_style_text_color(feedback.caption,lv_color_hex(color),0);
        feedback.styled=true; feedback.holding=holding; feedback.runnable=runnable;
    }
    char text[32];
    unsigned tenths=progress*HOLD_MS/10000U;
    if(holding) snprintf(text,sizeof(text),"HOLD %u.%u / %uS",tenths/10U,tenths%10U,HOLD_MS/1000U);
    else snprintf(text,sizeof(text),"%s",run_submitted || model.run_state==RYZ_V5_APPS_PREPARING?
        "PREPARING":runnable?"HOLD 2S TO RUN":"RUN UNAVAILABLE");
    if(strcmp(lv_label_get_text(feedback.caption),text)) lv_label_set_text(feedback.caption,text);
    if(strcmp(lv_label_get_text(feedback.fill_caption),text)) lv_label_set_text(feedback.fill_caption,text);
    lv_obj_set_width(feedback.bar,holding && progress?(int)(152U*progress/100U):0);
}

bool ryz_v5_apps_update(lv_obj_t *root)
{
    if(root && list_feedback && list_feedback->root==root &&
       list_feedback->revision==content_revision && list_feedback->error==local_error &&
       !await_catalog && !dialog && catalog() && list_valid && !relocating &&
       !window_pending && !window_failed && model.reader.state==RYZ_APPS_READY) {
        update_list(); dirty=false; return true;
    }
    if(!root || feedback.root!=root || !feedback.button || !feedback.caption || !feedback.bar ||
       feedback.revision!=content_revision ||
       feedback.error!=local_error || await_catalog || dialog || !ready_detail()) return false;
    if(feedback.scroll!=detail_scroll) {
        lv_obj_set_y(feedback.content,-34-detail_scroll);
        if(feedback.thumb) {
            int height=DETAIL_HEIGHT*DETAIL_HEIGHT/detail_content_height;
            if(height<4) height=4;
            int top=(DETAIL_HEIGHT-height)*detail_scroll/(detail_content_height-DETAIL_HEIGHT);
            lv_obj_set_y(feedback.thumb,34+top);
        }
        feedback.scroll=detail_scroll;
    }
    update_feedback();
    dirty=false;
    return true;
}

static void draw_detail(lv_obj_t *root)
{
    if (model.reader.state != RYZ_APPS_READY) {
        label(root, 12, 76, 216, 60, problem() ? problem() : "LOADING APPLICATION DETAILS",
              RYZ_FONT_BODY, 12, SECONDARY, LV_TEXT_ALIGN_CENTER);
        label(root, 12, 148, 216, 24, "BACK TO RESELECT", RYZ_FONT_MONO, 12, ORANGE, LV_TEXT_ALIGN_CENTER);
        return;
    }
    const ryz_apps_detail_t *detail = &model.reader.detail;
    char size[32], path[64], value[RYZ_SCRIPT_METADATA_DESCRIPTION_MAX + 1U];
    char description[RYZ_SCRIPT_METADATA_DESCRIPTION_MAX + 1U];
    ascii(description, sizeof(description), detail->metadata.description_invalid ? "INVALID METADATA" : detail->metadata.description);
    const lv_font_t *body = ryz_v5_font_get(RYZ_FONT_BODY, 12);
    if (!body) { draw_ok = false; return; }
    lv_point_t extent;
    lv_text_get_size(&extent, description, body, 0, 16 - (int)body->line_height, 216, LV_TEXT_FLAG_NONE);
    int comment_height = extent.y > 32 ? extent.y : 32;
    int extension = comment_height - 32;
    detail_content_height = 172 + extension;
    detail_scroll = clamp_scroll(detail_scroll, detail_content_height, DETAIL_HEIGHT);
    lv_obj_t *screen = root;
    lv_obj_t *viewport = ryz_v5_box(screen, 8, 34, 224, DETAIL_HEIGHT, BACKGROUND, 0, 0);
    if (!viewport) { draw_ok = false; return; }
    root = ryz_v5_box(viewport, -8, -34 - detail_scroll, 240, 34 + detail_content_height, BACKGROUND, 0, 0);
    if (!root) { draw_ok = false; return; }
    lv_obj_t *detail_content=root;
    lv_obj_t *detail_thumb=NULL;
    size_text(size, sizeof(size), detail->file.entry.bytes);
    label(root, 8, 34, 224, 14, detail->source_valid ? "LUA APP / VALID" : "LUA APP / INVALID SOURCE",
          RYZ_FONT_MONO_MEDIUM, 12, detail->source_valid ? V5_GREEN : ERROR_COLOR, LV_TEXT_ALIGN_LEFT);
    label(root, 8, 48, 164, 22, detail->file.entry.name, RYZ_FONT_DISPLAY, 20, WHITE, LV_TEXT_ALIGN_LEFT);
    label(root, 176, 50, 56, 18, size, RYZ_FONT_MONO_MEDIUM, 12, ORANGE, LV_TEXT_ALIGN_RIGHT);
    snprintf(path, sizeof(path), "/scripts/%s", detail->file.entry.name);
    label(root, 8, 70, 224, 16, path, RYZ_FONT_MONO, 12, SECONDARY, LV_TEXT_ALIGN_LEFT);
    box(root, 8, 88, 224, 52 + extension, SURFACE, BORDER, 1);
    label(root, 12, 90, 216, 14, "HEADER COMMENT", RYZ_FONT_MONO_MEDIUM, 12, ORANGE, LV_TEXT_ALIGN_LEFT);
    paragraph(root, 12, 104, 216, comment_height, description, 12, 16, WHITE);
    cell(root, 8, 144 + extension, "SIZE", size, WHITE);
    if (detail->metadata.version_invalid) ascii(value, sizeof(value), "INVALID");
    else display_version(value, sizeof(value), detail->metadata.version);
    cell(root, 83, 144 + extension, "VERSION", value, WHITE);
    ascii(value, sizeof(value), detail->metadata.author_invalid ? "INVALID" : detail->metadata.author);
    cell(root, 158, 144 + extension, "AUTHOR", value, WHITE);
    char created[6], modified[6];
    date_text(created, detail->file.times.created_unix);
    date_text(modified, detail->file.times.modified_unix);
    cell(root, 8, 176 + extension, "CREATED", created, WHITE);
    cell(root, 83, 176 + extension, "MODIFIED", modified, WHITE);
    cell(root, 158, 176 + extension, "STATUS", detail->source_valid ? "VALID" : "INVALID",
         detail->source_valid ? V5_GREEN : ERROR_COLOR);
    root = screen;
    if (detail_content_height > DETAIL_HEIGHT) {
        int height = DETAIL_HEIGHT * DETAIL_HEIGHT / detail_content_height;
        if (height < 4) height = 4;
        int top = (DETAIL_HEIGHT - height) * detail_scroll / (detail_content_height - DETAIL_HEIGHT);
        box(root, 234, 34, 2, DETAIL_HEIGHT, BORDER, BORDER, 0);
        detail_thumb=ryz_v5_box(root,234,34+top,2,height,ORANGE,ORANGE,0);
        if(!detail_thumb) draw_ok=false;
    }
    lv_obj_t *button=ryz_v5_box(root,80,200,152,36,ORANGE,ORANGE,1);
    if(button) lv_obj_set_style_radius(button,2,0);
    lv_obj_t *caption=label(root,80,208,152,20,"HOLD 2S TO RUN",
        RYZ_FONT_BODY_SEMIBOLD,16,0,LV_TEXT_ALIGN_CENTER);
    rounded_box(root, 8, 200, 68, 36, SURFACE, detail->file.entry.protected_file ? BORDER : ERROR_COLOR, 1, 4);
    label(root, 8, 208, 68, 20, "DELETE", RYZ_FONT_BODY_SEMIBOLD, 14,
          detail->file.entry.protected_file ? SECONDARY : ERROR_COLOR, LV_TEXT_ALIGN_CENTER);
    /* Clip a black copy of the caption to the growing orange background.
     * Both captions remain fixed: the fill never obscures or shifts text.
     * Allocate once, before presentation; no objects are created while held. */
    lv_obj_t *bar=ryz_v5_box(root,80,200,0,36,ORANGE,ORANGE,0);
    lv_obj_t *fill_caption=bar?label(bar,0,8,152,20,"HOLD 2S TO RUN",
        RYZ_FONT_BODY_SEMIBOLD,16,0,LV_TEXT_ALIGN_CENTER):NULL;
    if(bar) { lv_obj_set_style_radius(bar,2,0); lv_obj_set_style_clip_corner(bar,true,0); }
    if(!button || !caption || !bar || !fill_caption) draw_ok=false;
    else {
        feedback.root=root; feedback.button=button; feedback.caption=caption; feedback.bar=bar;
        feedback.fill_caption=fill_caption;
        feedback.content=detail_content; feedback.thumb=detail_thumb;
        feedback.styled=false;
        feedback.revision=content_revision; feedback.scroll=detail_scroll; feedback.error=local_error;
        lv_obj_add_event_cb(root,feedback_deleted,LV_EVENT_DELETE,NULL);
        update_feedback();
    }
    const char *status = problem();
    if (status && model.run_state != RYZ_V5_APPS_PREPARING) {
        box(root, 8, 156, 224, 36, BACKGROUND, ERROR_COLOR, 1);
        paragraph(root, 12, 158, 216, 32, status, 12, 16, ERROR_COLOR);
    }
}

static void draw_dialog(lv_obj_t *root)
{
    box(root, 0, 32, 240, 208, 0, 0, 0);
    rounded_box(root, 12, 44, 216, 184, SURFACE, ERROR_COLOR, 1, 4);
    box(root, 12, 44, 4, 184, ERROR_COLOR, ERROR_COLOR, 0);
    label(root, 28, 52, 184, 16, "DESTRUCTIVE ACTION", RYZ_FONT_MONO_MEDIUM, 12, ERROR_COLOR, LV_TEXT_ALIGN_LEFT);
    label(root, 28, 72, 184, 24, "DELETE SCRIPT?", RYZ_FONT_DISPLAY, 22, WHITE, LV_TEXT_ALIGN_LEFT);
    label(root, 28, 98, 184, 18, dialog_name, RYZ_FONT_MONO, 12, SECONDARY, LV_TEXT_ALIGN_LEFT);
    paragraph(root, 28, 122, 184, 48,
              "Removes this Lua file from the device filesystem.\nThis action cannot be undone.",
              12, 16, 0xd9d9d9);
    box(root, 28, 178, 184, 1, BORDER, BORDER, 0);
    rounded_box(root, 24, 190, 88, 30, SURFACE, BORDER, 1, 4);
    label(root, 24, 190, 88, 30, "CANCEL", RYZ_FONT_BODY_SEMIBOLD, 14, WHITE, LV_TEXT_ALIGN_CENTER);
    rounded_box(root, 120, 190, 96, 30, ERROR_COLOR, ERROR_COLOR, 1, 4);
    label(root, 120, 190, 96, 30, "DELETE", RYZ_FONT_BODY_SEMIBOLD, 14, 0, LV_TEXT_ALIGN_CENTER);
}

esp_err_t ryz_v5_apps_draw(lv_obj_t *root)
{
    if (!root) return ESP_ERR_INVALID_ARG;
    draw_ok = true;
    box(root, 0, 32, 240, ryz_v5_apps_has_navigation() ? 164 : 208, BACKGROUND, 0, 0);
    if (await_catalog) draw_list(root);
    else if (dialog) draw_dialog(root);
    else if (catalog()) draw_list(root);
    else draw_detail(root);
    esp_err_t error = ryz_v5_widgets_status();
    if (error == ESP_OK && !draw_ok) error = ESP_ERR_NO_MEM;
    if (error == ESP_OK) dirty = false;
    return error;
}
