#include "ryz_v5_widgets.h"
#include "ryz_v5_font.h"
#include <string.h>

/* Only a small lookup/usage table; all system glyphs and metrics are static.
 * The dynamic FreeType adapter remains exclusively on the Lua UI path. */
static struct {
    ryz_font_face_t face;
    unsigned px;
    const lv_font_t *font;
} fonts[48]; /* Finite static palette, including the V5 readability pass. */
static unsigned font_count;
static esp_err_t failure;

/* Metadata lives in the bounded LVGL pool (PSRAM on target), not internal
 * static RAM. There is one current page, not one cached tree per route. */
typedef struct {
    lv_obj_t *object, *parent;
    const void *resource;
    int x,y,w,h,border_px;
    uint32_t color,border;
    uint8_t kind,align;
} declaration_t;
enum { DECLARATIONS_MAX=160, DECL_BOX, DECL_LABEL, DECL_IMAGE };
static struct {
    lv_obj_t *root;
    declaration_t *items;
    unsigned count,cursor;
    bool recording,reuse,mismatch;
} retained;

static void retained_deleted(lv_event_t *event)
{
    if(lv_event_get_target_obj(event)!=retained.root) return;
    lv_free(retained.items);
    memset(&retained,0,sizeof(retained));
}

esp_err_t ryz_v5_widgets_retain_begin(lv_obj_t *root,bool reuse)
{
    if(!root) return ESP_ERR_INVALID_ARG;
    if(reuse && (retained.root!=root || !retained.items)) return ESP_ERR_INVALID_STATE;
    if(!reuse) {
        lv_free(retained.items);
        memset(&retained,0,sizeof(retained));
        retained.root=root;
        retained.items=lv_malloc(sizeof(*retained.items)*DECLARATIONS_MAX);
        if(!retained.items) { memset(&retained,0,sizeof(retained)); return ESP_ERR_NO_MEM; }
        memset(retained.items,0,sizeof(*retained.items)*DECLARATIONS_MAX);
        if(!lv_obj_add_event_cb(root,retained_deleted,LV_EVENT_DELETE,NULL)) {
            lv_free(retained.items); memset(&retained,0,sizeof(retained));
            return ESP_ERR_NO_MEM;
        }
    }
    retained.cursor=0; retained.recording=true; retained.reuse=reuse; retained.mismatch=false;
    return ESP_OK;
}

esp_err_t ryz_v5_widgets_retain_end(void)
{
    if(retained.reuse && retained.cursor!=retained.count) retained.mismatch=true;
    retained.recording=false;
    return retained.mismatch ? ESP_ERR_NOT_SUPPORTED : ryz_v5_widgets_status();
}

static declaration_t *declaration(const declaration_t *spec)
{
    if(!retained.recording) return NULL;
    if(retained.cursor==DECLARATIONS_MAX ||
       (retained.reuse && retained.cursor>=retained.count)) {
        retained.mismatch=true; failure=ESP_ERR_NOT_SUPPORTED; return NULL;
    }
    declaration_t *slot=&retained.items[retained.cursor++];
    if(retained.reuse && (slot->parent!=spec->parent || slot->kind!=spec->kind)) {
        retained.mismatch=true; failure=ESP_ERR_NOT_SUPPORTED; return NULL;
    }
    if(!retained.reuse) retained.count=retained.cursor;
    return slot;
}

static bool identical(const declaration_t *a,const declaration_t *b)
{
    return a && a->object && a->x==b->x && a->y==b->y && a->w==b->w && a->h==b->h &&
        a->resource==b->resource && a->border_px==b->border_px && a->color==b->color &&
        a->border==b->border && a->align==b->align;
}

void ryz_v5_radius(lv_obj_t *object,int radius)
{
    if(object && lv_obj_get_style_radius(object,0)!=radius)
        lv_obj_set_style_radius(object,radius,0);
}

void ryz_v5_long_mode(lv_obj_t *object,lv_label_long_mode_t mode)
{
    if(object && lv_label_get_long_mode(object)!=mode) lv_label_set_long_mode(object,mode);
}

void ryz_v5_widgets_begin(void)
{ failure = ESP_OK; retained.recording=false; ryz_v5_font_begin(); }
unsigned ryz_v5_widgets_font_count(void) { return font_count; }

esp_err_t ryz_v5_widgets_status(void)
{
    if (failure != ESP_OK) return failure;
    return ryz_v5_font_status();
}

void ryz_v5_widgets_release(void)
{
    for (unsigned i = 0; i < font_count; ++i) {
        fonts[i].font = NULL;
    }
    font_count = 0;
    failure = ESP_OK;
    ryz_v5_font_begin();
}

static const lv_font_t *font_get(ryz_font_face_t face, unsigned px)
{
    for (unsigned i = 0; i < font_count; ++i)
        if (fonts[i].face == face && fonts[i].px == px)
            return fonts[i].font;
    if (font_count == sizeof(fonts) / sizeof(fonts[0])) {
        failure = ESP_ERR_NO_MEM;
        return NULL;
    }
    const lv_font_t *font = ryz_v5_font_get(face, px);
    if (!font) { failure = ryz_v5_font_status(); return NULL; }
    fonts[font_count].face = face;
    fonts[font_count].px = px;
    fonts[font_count++].font = font;
    return font;
}

static lv_obj_t *plain(lv_obj_t *object, int x, int y, int w, int h)
{
    if (!object) { failure = ESP_ERR_NO_MEM; return NULL; }
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    return object;
}

lv_obj_t *ryz_v5_box(lv_obj_t *root, int x, int y, int w, int h,
                     uint32_t bg, uint32_t border, int border_px)
{
    if (!root || failure != ESP_OK) return NULL;
    declaration_t spec={.parent=root,.kind=DECL_BOX,.x=x,.y=y,.w=w,.h=h,
        .color=bg,.border=border,.border_px=border_px};
    declaration_t *slot=declaration(&spec);
    if(failure!=ESP_OK) return NULL;
    if(identical(slot,&spec)) return slot->object;
    lv_obj_t *object = plain(slot && slot->object?slot->object:lv_obj_create(root), x, y, w, h);
    if (!object) return NULL;
    lv_obj_set_style_bg_color(object, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    if (border_px > 0) {
        lv_obj_set_style_border_color(object, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(object, border_px, 0);
        lv_obj_set_style_border_opa(object, LV_OPA_COVER, 0);
    }
    if(slot) { spec.object=object; *slot=spec; }
    return object;
}

lv_obj_t *ryz_v5_label(lv_obj_t *root, int x, int y, int w, int h,
                       const char *text, ryz_font_face_t face, unsigned px,
                       uint32_t color, lv_text_align_t align)
{
    if (!root || !text || failure != ESP_OK) return NULL;
    const lv_font_t *font = font_get(face, px);
    if (!font) return NULL;
    declaration_t spec={.parent=root,.kind=DECL_LABEL,.x=x,.y=y,.w=w,.h=h,
        .color=color,.resource=font,.align=(uint8_t)align};
    declaration_t *slot=declaration(&spec);
    if(failure!=ESP_OK) return NULL;
    if(identical(slot,&spec)) {
        if(strcmp(lv_label_get_text(slot->object),text)) lv_label_set_text(slot->object,text);
        return slot->object;
    }
    lv_obj_t *object = plain(slot && slot->object?slot->object:lv_label_create(root), x,
        y + (h - (int)font->line_height) / 2, w, font->line_height);
    if (!object) return NULL;
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, align, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    lv_label_set_text(object, text);
    if(slot) { spec.object=object; *slot=spec; }
    return object;
}

lv_obj_t *ryz_v5_image(lv_obj_t *root, int x, int y,
                       const lv_image_dsc_t *asset, uint32_t color)
{
    if (!root || !asset || failure != ESP_OK) return NULL;
    declaration_t spec={.parent=root,.kind=DECL_IMAGE,.x=x,.y=y,
        .w=asset->header.w,.h=asset->header.h,.resource=asset,.color=color};
    declaration_t *slot=declaration(&spec);
    if(failure!=ESP_OK) return NULL;
    if(identical(slot,&spec)) return slot->object;
    lv_obj_t *object = plain(slot && slot->object?slot->object:lv_image_create(root), x, y,
                              asset->header.w, asset->header.h);
    if (!object) return NULL;
    lv_image_set_src(object, asset);
    lv_obj_set_style_image_recolor(object, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(object, LV_OPA_COVER, 0);
    if(slot) { spec.object=object; *slot=spec; }
    return object;
}

static int info_value_height(const char *value)
{
    const lv_font_t *font = ryz_v5_font_get(RYZ_FONT_BODY, 14);
    if (!font || !value) return 20;
    lv_point_t size;
    lv_text_get_size(&size, value, font, 0, 20 - (int)font->line_height,
                     152, LV_TEXT_FLAG_NONE);
    return size.y > 20 ? ((size.y + 19) / 20) * 20 : 20;
}

int ryz_v5_info_row_y(const ryz_v5_info_field_t *fields, size_t index)
{
    int y = 0;
    for (size_t i = 0; i < index; ++i)
        y += fields[i].label ? info_value_height(fields[i].value) + 16 : 24;
    return y;
}

int ryz_v5_info_height(const ryz_v5_info_field_t *fields, size_t count)
{ return ryz_v5_info_row_y(fields, count); }

lv_obj_t *ryz_v5_info_draw(lv_obj_t *root, const ryz_v5_info_field_t *fields,
                          size_t count, int offset)
{
    int height = ryz_v5_info_height(fields, count);
    int maximum = height > 132 ? height - 132 : 0;
    if (offset < 0) offset = 0;
    if (offset > maximum) offset = maximum;
    lv_obj_t *viewport = ryz_v5_box(root, 8, 58, 224, 132, V5_BLACK, 0, 0);
    if (!viewport) return NULL;
    lv_obj_t *content = ryz_v5_box(viewport, 0, -offset, 224, height, V5_BLACK, 0, 0);
    if (!content) return NULL;
    int y = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!fields[i].label) {
            ryz_v5_label(content,0,y,224,24,fields[i].value,RYZ_FONT_BODY,12,V5_MUTED,LV_TEXT_ALIGN_LEFT);
            y += 24;
            continue;
        }
        int h = info_value_height(fields[i].value);
        ryz_v5_label(content, 4, y + 8, 60, 20, fields[i].label,
                     RYZ_FONT_BODY, 12, V5_MUTED, LV_TEXT_ALIGN_LEFT);
        if (fields[i].value) {
            lv_obj_t *label = ryz_v5_label(content, 68, y + 8, 152, 20,
                fields[i].value, RYZ_FONT_BODY, 14, V5_BODY, LV_TEXT_ALIGN_LEFT);
            if (label) {
                const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
                if(lv_obj_get_style_text_line_space(label,0)!=20-(int)font->line_height)
                    lv_obj_set_style_text_line_space(label, 20 - (int)font->line_height, 0);
                ryz_v5_long_mode(label, LV_LABEL_LONG_WRAP);
                lv_obj_set_height(label, h);
            }
        }
        y += h + 16;
        ryz_v5_box(content, 0, y - 1, 224, 1, V5_BORDER, 0, 0);
    }
    if (maximum) {
        int thumb = 132 * 132 / height;
        if (thumb < 12) thumb = 12;
        ryz_v5_box(root, 234, 58, 2, 132, V5_BORDER, 0, 0);
        ryz_v5_box(root, 234, 58 + offset * (132 - thumb) / maximum,
                   2, thumb, V5_MUTED, 0, 0);
    }
    return content;
}
