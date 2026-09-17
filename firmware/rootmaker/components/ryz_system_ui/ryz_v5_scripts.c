#include "ryz_v5_scripts.h"
#include "ryz_v5_assets.h"
#include "ryz_v5_widgets.h"

#include <stdio.h>
#include <string.h>

/* Original 240px geometry: 92:3073, 92:3112, 92:3156, 92:3187.
 * This module has no stateful cache, filesystem calls or success transition. */
static bool draw_ok;

static bool page_valid(const ryz_v5_scripts_snapshot_t *s)
{ return s && s->page >= RYZ_V5_SCRIPTS_SETTINGS && s->page <= RYZ_V5_SCRIPTS_DELETE_ALL; }

static bool catalog_ready(const ryz_v5_scripts_snapshot_t *s)
{
    return s->store_ready && !s->recovery_required &&
        s->catalog.count <= RYZ_SCRIPT_STORE_PAGE_MAX &&
        s->catalog.total <= RYZ_SCRIPT_STORE_INDEX_MAX &&
        s->catalog.offset <= s->catalog.total &&
        s->catalog.count <= s->catalog.total - s->catalog.offset &&
        (s->catalog_state == RYZ_APPS_READY || s->catalog_state == RYZ_APPS_EMPTY);
}

static bool operation_idle(const ryz_v5_scripts_snapshot_t *s)
{ return s->result == RYZ_V5_SCRIPTS_IDLE || s->result == RYZ_V5_SCRIPTS_FAILED; }

static bool can_browse(const ryz_v5_scripts_snapshot_t *s)
{ return s->result != RYZ_V5_SCRIPTS_PENDING && s->result != RYZ_V5_SCRIPTS_UNKNOWN; }

static bool name_valid(const char *name)
{
    size_t i = 0;
    for (; i <= RYZ_SCRIPT_STORE_NAME_MAX && name[i]; ++i)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126) return false;
    return i > 0 && i <= RYZ_SCRIPT_STORE_NAME_MAX;
}

static bool selection_valid(const ryz_v5_scripts_snapshot_t *s)
{
    if (!catalog_ready(s) || !s->selected_identity_valid || !s->catalog.revision ||
        s->selected_revision != s->catalog.revision || !name_valid(s->selected.entry.name)) return false;
    for (unsigned i = 0; i < 64; ++i) {
        char c = s->selected.sha256[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s->selected.sha256[64] == 0;
}

static bool saved(const ryz_v5_scripts_snapshot_t *s)
{
    return s->operation == RYZ_V5_SCRIPTS_OP_BOOT && s->result == RYZ_V5_SCRIPTS_COMMITTED &&
        s->error == ESP_OK && name_valid(s->result_name);
}

static bool can_apply(const ryz_v5_scripts_snapshot_t *s)
{ return s->generation && s->can_change_boot && s->selected_source_valid && selection_valid(s) && operation_idle(s); }

static bool can_delete(const ryz_v5_scripts_snapshot_t *s)
{
    for (unsigned i = 0; i < 64; ++i) {
        char c = s->boot_sha256[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s->generation && s->can_clear_boot && catalog_ready(s) && s->boot_known &&
        !strcmp(s->boot_name, "boot.lua") && s->boot_revision == s->catalog.revision &&
        s->boot_sha256[64] == 0 &&
        (s->page == RYZ_V5_SCRIPTS_SETTINGS ? can_browse(s) : operation_idle(s));
}

static void text(lv_obj_t *root, int x, int y, int w, int h, const char *value,
                  ryz_font_face_t face, unsigned px, uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = ryz_v5_label(root, x, y, w, h, value, face, px, color, align);
    if (!label) draw_ok = false;
    else lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static void box(lv_obj_t *root, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *object = ryz_v5_box(root, x, y, w, h, color, 0, 0);
    if (!object) draw_ok = false;
    else lv_obj_set_style_radius(object, radius, 0);
}

static void body(lv_obj_t *root, int x, int y, int w, int h, const char *value,
                  uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = ryz_v5_label(root, x, y, w, h, value, RYZ_FONT_BODY_MEDIUM,12, color, align);
    if (!label) { draw_ok = false; return; }
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_point_t size;
    lv_text_get_size(&size, value, font, 0, 0, w, LV_TEXT_FLAG_NONE);
    lv_obj_set_pos(label, x, y + (h - size.y) / 2);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static void button(lv_obj_t *root, int x, int y, int w, int h,
                    const char *label, uint32_t color, bool enabled)
{
    box(root, x, y, w, h, enabled ? color : V5_SELECTED, 2);
    text(root, x, y, w, h, label, RYZ_FONT_BODY_SEMIBOLD,14,
         enabled && color != V5_SELECTED ? V5_BLACK : V5_BODY, LV_TEXT_ALIGN_CENTER);
}

static void size_text(char out[32], size_t bytes)
{
    if (bytes < 1024U) snprintf(out, 32, "%zu B", bytes);
    else snprintf(out, 32, "%zu.%zu KB", bytes / 1024U, (bytes % 1024U) * 10U / 1024U);
}

static const char *failure(const ryz_v5_scripts_snapshot_t *s)
{
    if (s->result == RYZ_V5_SCRIPTS_UNKNOWN) return "OUTCOME UNKNOWN - DO NOT RETRY";
    if (s->result == RYZ_V5_SCRIPTS_PENDING) return "OPERATION PENDING";
    if (s->recovery_required) return "STORAGE RECOVERY REQUIRED";
    if (s->error != ESP_OK || s->result == RYZ_V5_SCRIPTS_FAILED) return "OPERATION FAILED";
    return NULL;
}

static void footer(lv_obj_t *root, const char *value)
{
    box(root, 0, 216, 240, 24, V5_BLACK, 0);
    box(root, 0, 216, 240, 1, V5_BORDER, 0);
    text(root, 8, 216, 224, 24, value, RYZ_FONT_MONO_MEDIUM,12, V5_MUTED, LV_TEXT_ALIGN_RIGHT);
}

static void settings(lv_obj_t *root, const ryz_v5_scripts_snapshot_t *s)
{
    char value[64];
    text(root, 8, 36, 120, 18, "STARTUP SCRIPT", RYZ_FONT_MONO_SEMIBOLD,12, V5_ORANGE, LV_TEXT_ALIGN_LEFT);
    box(root, 176, 37, 56, 16, V5_SELECTED, 2);
    if (catalog_ready(s)) snprintf(value, sizeof(value), "%zu LUA", s->catalog.total);
    else snprintf(value, sizeof(value), "- LUA");
    text(root, 176, 37, 56, 16, value, RYZ_FONT_MONO_SEMIBOLD,12, V5_ORANGE, LV_TEXT_ALIGN_CENTER);
    box(root, 8, 60, 224, 62, V5_SELECTED, 2);
    box(root, 8, 60, 3, 62, V5_ORANGE, 0);
    text(root, 16, 64, 100, 18, "BOOT SCRIPT", RYZ_FONT_MONO_SEMIBOLD,12, V5_MUTED, LV_TEXT_ALIGN_LEFT);
    text(root, 16, 82, 156, 28, !s->boot_known ? "UNKNOWN" :
         !s->boot_name[0] ? "NONE" : name_valid(s->boot_name) ? s->boot_name : "UNAVAILABLE",
         RYZ_FONT_MONO_MEDIUM, 15, V5_WHITE, LV_TEXT_ALIGN_LEFT);
    if (s->boot_known && s->boot_bytes_known) size_text(value, s->boot_bytes);
    else snprintf(value, sizeof(value), "-");
    text(root, 176, 86, 48, 20, value, RYZ_FONT_MONO,12, V5_MUTED, LV_TEXT_ALIGN_RIGHT);
    box(root, 8, 128, 224, 34, V5_SELECTED, 0);
    box(root, 8, 128, 3, 34, V5_ORANGE, 0);
    box(root, 8, 161, 224, 1, V5_BORDER, 0);
    text(root, 16, 128, 144, 34, "CHANGE BOOT SCRIPT", RYZ_FONT_BODY_SEMIBOLD,12, V5_BODY, LV_TEXT_ALIGN_LEFT);
    bool open = s->can_open_picker || s->can_change_boot;
    text(root, 164, 128, 60, 34, open ? "OPEN" : "N/A",
         RYZ_FONT_MONO_SEMIBOLD,14, open ? V5_ORANGE : V5_MUTED, LV_TEXT_ALIGN_RIGHT);
    box(root, 8, 162, 1, 34, V5_BORDER, 0);
    box(root, 8, 195, 224, 1, V5_BORDER, 0);
    text(root, 16, 162, 164, 34, "CLEAR BOOT SCRIPT", RYZ_FONT_BODY_MEDIUM,14, V5_BODY, LV_TEXT_ALIGN_LEFT);
    text(root, 180, 162, 44, 34, can_delete(s) ? "OPEN" :
         s->boot_known && !s->boot_name[0] ? "NONE" : "N/A", RYZ_FONT_BODY_MEDIUM,14,
         V5_MUTED, LV_TEXT_ALIGN_RIGHT);
    footer(root, failure(s) ? failure(s) : open ? "COPIES AS boot.lua" : "STORAGE UNAVAILABLE");
}

static void picker(lv_obj_t *root, const ryz_v5_scripts_snapshot_t *s)
{
    char value[64];
    text(root, 8, 34, 132, 18, "SELECT SCRIPT", RYZ_FONT_BODY,12, V5_ORANGE, LV_TEXT_ALIGN_LEFT);
    if (catalog_ready(s)) snprintf(value, sizeof(value), "%zu TOTAL", s->catalog.total);
    else snprintf(value, sizeof(value), "- TOTAL");
    text(root, 152, 34, 80, 18, value, RYZ_FONT_BODY,12, V5_MUTED, LV_TEXT_ALIGN_RIGHT);
    lv_obj_t *viewport = ryz_v5_box(root, 8, 56, 224, 134, V5_BLACK, 0, 0);
    if (!viewport) { draw_ok = false; return; }
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
    if (catalog_ready(s)) {
        for (size_t i = 0; i < s->catalog.count; ++i) {
            int y = (int)(s->catalog.offset + i) * 31 - s->scroll_y;
            if (y + 30 <= 0 || y >= 134) continue;
            const ryz_script_store_entry_t *entry = &s->catalog.entries[i];
            bool selected = name_valid(entry->name) && selection_valid(s) &&
                !strcmp(entry->name, s->selected.entry.name);
            if (selected) box(viewport, 0, y, 224, 30, V5_SELECTED, 0);
            if (selected) box(viewport, 0, y, 2, 30, V5_ORANGE, 0);
            box(viewport, 12, y + 9, 10, 10, selected ? V5_ORANGE : V5_MUTED, 5);
            if (!selected) box(viewport, 13, y + 10, 8, 8, V5_BLACK, 4);
            text(viewport, 34, y + 4, 106, 20, name_valid(entry->name) ? entry->name : "INVALID NAME",
                 RYZ_FONT_BODY_SEMIBOLD,14, V5_BODY, LV_TEXT_ALIGN_LEFT);
            if(entry->bytes>=1024) snprintf(value,sizeof(value),"%zu.%zuK",entry->bytes/1024,entry->bytes%1024*10/1024);
            else snprintf(value,sizeof(value),"%zuB",entry->bytes);
            text(viewport, 142, y + 5, 40, 18, value, RYZ_FONT_BODY,12, V5_MUTED, LV_TEXT_ALIGN_RIGHT);
            text(viewport, 184, y + 5, 34, 18, "LUA", RYZ_FONT_BODY,12, V5_MUTED, LV_TEXT_ALIGN_RIGHT);
            box(viewport, 0, y + 30, 224, 1, V5_BORDER, 0);
        }
        if (!s->catalog.total) body(viewport, 0, 32, 224, 60, "NO LUA FILES", V5_MUTED, LV_TEXT_ALIGN_CENTER);
        const int height = (int)s->catalog.total * 31 - 1;
        if (height > 134) {
            int thumb = 134 * 134 / height;
            if (thumb < 8) thumb = 8;
            int top = s->scroll_y * (134 - thumb) / (height - 134);
            box(root, 234, 56, 2, 134, V5_BORDER, 0);
            box(root, 234, 56 + top, 2, thumb, V5_ORANGE, 0);
        }
    } else body(viewport, 0, 32, 224, 60, s->catalog_state == RYZ_APPS_LOADING ? "LOADING FILES" : "FILES UNAVAILABLE",
                V5_MUTED, LV_TEXT_ALIGN_CENTER);
    button(root, 8, 194, 224, 42, s->result == RYZ_V5_SCRIPTS_PENDING ? "SAVING..." :
           s->result == RYZ_V5_SCRIPTS_UNKNOWN ? "CHECK REQUIRED" :
           s->result == RYZ_V5_SCRIPTS_FAILED ? "SAVE FAILED" :
           "SET AS BOOT SCRIPT", V5_ORANGE, can_apply(s));
}

static void saved_page(lv_obj_t *root, const ryz_v5_scripts_snapshot_t *s)
{
    bool committed = saved(s);
    box(root, 8, 38, 124, 16, V5_SELECTED, 2);
    text(root, 8, 38, 124, 16, committed ? "BOOT SCRIPT SET" : "NOT CONFIRMED",
         RYZ_FONT_MONO_SEMIBOLD,12, committed ? V5_GREEN : V5_MUTED, LV_TEXT_ALIGN_CENTER);
    if (committed) {
        if (!ryz_v5_image(root, 104, 72, &ryz_v5_asset_scripts_saved, V5_GREEN)) draw_ok = false;
    } else body(root, 24, 62, 192, 48, failure(s) ? failure(s) : "NO COMMITTED BOOT CHANGE",
                V5_MUTED, LV_TEXT_ALIGN_CENTER);
    text(root, 24, 116, 192, 28, committed ? s->result_name : "-", RYZ_FONT_MONO_MEDIUM, 16, V5_WHITE, LV_TEXT_ALIGN_CENTER);
    body(root, 28, 146, 184, 34, committed ? "Restart the device to run the new boot script." : "The current boot configuration has not been changed.",
         committed ? V5_WARNING : V5_MUTED, LV_TEXT_ALIGN_CENTER);
    button(root, 8, 184, 76, 28, "LATER", V5_SELECTED, true);
    button(root, 88, 184, 144, 28, "RESTART NOW", V5_ORANGE, committed && s->can_restart);
    footer(root, committed ? "CHANGES SAVED" : failure(s) ? failure(s) : "NO CHANGES SAVED");
}

static void delete_page(lv_obj_t *root, const ryz_v5_scripts_snapshot_t *s)
{
    const bool started = s->operation == RYZ_V5_SCRIPTS_OP_CLEAR_BOOT && s->result != RYZ_V5_SCRIPTS_IDLE;
    const char *title = !s->boot_known ? "CHECK REQUIRED" : !s->boot_name[0] ? "NO BOOT SCRIPT" : "CLEAR BOOT SCRIPT?";
    const char *message = "Remove only boot.lua.\nOther scripts are kept.\nNo autostart next boot.";
    if (started) {
        title = s->result == RYZ_V5_SCRIPTS_PENDING ? "CLEARING..." :
            s->result == RYZ_V5_SCRIPTS_UNKNOWN ? "CHECK REQUIRED" :
            s->result == RYZ_V5_SCRIPTS_COMMITTED ? "BOOT SCRIPT CLEARED" :
            s->error == ESP_ERR_NOT_FOUND ? "NO BOOT SCRIPT" : "CLEAR FAILED";
        if (s->result == RYZ_V5_SCRIPTS_UNKNOWN)
            message = "Check storage state.\nDo not repeat this write.\nOther scripts are kept.";
        else if (s->result == RYZ_V5_SCRIPTS_COMMITTED)
            message = "No user boot.lua.\nOther scripts are kept.\nNo automatic restart.";
    }
    box(root, 8, 38, 136, 16, V5_SELECTED, 2);
    text(root, 8, 38, 136, 16, "boot.lua ONLY", RYZ_FONT_MONO_SEMIBOLD,12, V5_RED, LV_TEXT_ALIGN_CENTER);
    box(root, 8, 62, 224, 106, V5_SELECTED, 2);
    box(root, 8, 62, 3, 106, V5_RED, 0);
    text(root, 18, 68, 202, 28, title, RYZ_FONT_DISPLAY,20, V5_RED, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *label = ryz_v5_label(root, 18, 98, 202, 60, message, RYZ_FONT_BODY,14, V5_BODY, LV_TEXT_ALIGN_LEFT);
    if (!label) draw_ok = false;
    else {
        const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
        lv_obj_set_pos(label, 18, 98 + (20 - (int)font->line_height) / 2);
        lv_obj_set_size(label, 202, 60);
        lv_obj_set_style_text_line_space(label, 20 - (int)font->line_height, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
    button(root, 8, 180, 92, 28, started ? "BACK" : "CANCEL", V5_SELECTED, true);
    button(root, 104, 180, 128, 28, "CLEAR BOOT", V5_RED, !started && can_delete(s));
    footer(root, s->recovery_required ? "STORAGE RECOVERY REQUIRED" : "OTHER SCRIPTS KEPT");
}

const char *ryz_v5_scripts_title(const ryz_v5_scripts_snapshot_t *s)
{
    if (!page_valid(s)) return "SCRIPTS";
    return s->page == RYZ_V5_SCRIPTS_CLEAR_BOOT ? "CLEAR BOOT" :
        s->page == RYZ_V5_SCRIPTS_SETTINGS ? "SCRIPTS" : "BOOT";
}

esp_err_t ryz_v5_scripts_draw(lv_obj_t *root, const ryz_v5_scripts_snapshot_t *s)
{
    if (!root || !page_valid(s)) return ESP_ERR_INVALID_ARG;
    draw_ok = true;
    box(root, 0, 32, 240, 208, V5_BLACK, 0);
    if (s->page == RYZ_V5_SCRIPTS_SETTINGS) settings(root, s);
    else if (s->page == RYZ_V5_SCRIPTS_PICKER) picker(root, s);
    else if (s->page == RYZ_V5_SCRIPTS_SAVED) saved_page(root, s);
    else delete_page(root, s);
    esp_err_t error = ryz_v5_widgets_status();
    return error != ESP_OK ? error : draw_ok ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool inside(uint16_t x, uint16_t y, unsigned left, unsigned top, unsigned w, unsigned h)
{ return x >= left && y >= top && x < left + w && y < top + h; }

static void intent(const ryz_v5_scripts_snapshot_t *s, ryz_v5_scripts_intent_t *out,
                     ryz_v5_scripts_action_t action)
{
    if (!s->generation) return;
    out->action = action;
    out->generation = s->generation;
    out->revision = catalog_ready(s) ? s->catalog.revision : 0;
}

bool ryz_v5_scripts_hit(const ryz_v5_scripts_snapshot_t *s, uint16_t x, uint16_t y,
                         ryz_v5_scripts_intent_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !page_valid(s) || x >= 240 || y >= 240) return false;
    if (inside(x, y, 4, 0, 40, 32)) { intent(s, out, RYZ_V5_SCRIPTS_ACTION_BACK); return true; }
    if (s->page == RYZ_V5_SCRIPTS_SETTINGS) {
        if (inside(x, y, 8, 128, 224, 34)) {
            if ((s->can_open_picker || s->can_change_boot) && can_browse(s)) intent(s, out, RYZ_V5_SCRIPTS_ACTION_PICKER);
            return true;
        }
        if (inside(x, y, 8, 162, 224, 34)) {
            if (can_delete(s)) intent(s, out, RYZ_V5_SCRIPTS_ACTION_DELETE_CONFIRM);
            return true;
        }
    } else if (s->page == RYZ_V5_SCRIPTS_PICKER) {
        if (inside(x, y, 8, 56, 224, 134) && catalog_ready(s) && can_browse(s)) {
            const size_t absolute = (size_t)((int)y - 56 + s->scroll_y) / 31;
            if (absolute >= s->catalog.offset && absolute < s->catalog.offset + s->catalog.count &&
                ((int)y - 56 + s->scroll_y) % 31 < 30) {
                const size_t i = absolute - s->catalog.offset;
                if (s->generation && name_valid(s->catalog.entries[i].name)) {
                    intent(s, out, RYZ_V5_SCRIPTS_ACTION_SELECT);
                    out->index = i;
                    snprintf(out->name, sizeof(out->name), "%s", s->catalog.entries[i].name);
                }
            }
            return true;
        }
        if (inside(x, y, 8, 194, 224, 42)) {
            if (can_apply(s)) {
                intent(s, out, RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT);
                snprintf(out->name, sizeof(out->name), "%s", s->selected.entry.name);
                memcpy(out->sha256, s->selected.sha256, sizeof(out->sha256));
            }
            return true;
        }
    } else if (s->page == RYZ_V5_SCRIPTS_SAVED) {
        if (inside(x, y, 8, 184, 76, 28)) { intent(s, out, RYZ_V5_SCRIPTS_ACTION_LATER); return true; }
        if (inside(x, y, 88, 184, 144, 28)) {
            if (saved(s) && s->can_restart) intent(s, out, RYZ_V5_SCRIPTS_ACTION_RESTART);
            return true;
        }
    } else {
        if (inside(x, y, 8, 180, 92, 28)) { intent(s, out, RYZ_V5_SCRIPTS_ACTION_CANCEL); return true; }
        if (inside(x, y, 104, 180, 128, 28)) {
            if (can_delete(s)) {
                intent(s, out, RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT);
                strcpy(out->name, "boot.lua");
                memcpy(out->sha256, s->boot_sha256, sizeof(out->sha256));
            }
            return true;
        }
    }
    return false;
}

bool ryz_v5_scripts_scroll(const ryz_v5_scripts_snapshot_t *s, int delta_y, ryz_v5_scripts_intent_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !page_valid(s) || s->page != RYZ_V5_SCRIPTS_PICKER || !s->generation ||
        !catalog_ready(s) || !can_browse(s) || !delta_y || s->scroll_y < 0) return false;
    int maximum = (int)s->catalog.total * 31 - 1 - 134;
    if (maximum < 0) maximum = 0;
    int64_t desired = (int64_t)s->scroll_y + delta_y;
    int offset = desired < 0 ? 0 : desired > maximum ? maximum : (int)desired;
    if (offset == s->scroll_y) return false;
    size_t first = (size_t)offset / 31;
    size_t end = (size_t)(offset + 134 + 30) / 31;
    if (end > s->catalog.total) end = s->catalog.total;
    size_t window = s->catalog.offset;
    if (first < window || end > window + s->catalog.count) {
        window = first > 3 ? first - 3 : 0;
        if (s->catalog.total > RYZ_SCRIPT_STORE_PAGE_MAX &&
            window > s->catalog.total - RYZ_SCRIPT_STORE_PAGE_MAX)
            window = s->catalog.total - RYZ_SCRIPT_STORE_PAGE_MAX;
    }
    intent(s, out, RYZ_V5_SCRIPTS_ACTION_PAGE);
    out->offset = window; out->limit = RYZ_SCRIPT_STORE_PAGE_MAX; out->scroll_y = offset;
    return true;
}
