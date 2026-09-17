#include "ryz_v5_password.h"

#include <string.h>
#include "display.h"
#include "ryz_v5_widgets.h"

#define PASSWORD_CAPACITY 64U

static ryz_v5_password_binding_t binding;
static ryz_v5_password_metadata_t metadata;
static bool held, tainted;
static esp_err_t read_error;
/* Private static text, never copied to LVGL heap; revoked on release. */
static char text[PASSWORD_CAPACITY + 1U];

static void wipe(void *data, size_t size)
{
    volatile unsigned char *bytes=data;
    while(size--) *bytes++=0;
}

bool ryz_v5_password_revoke(void)
{
    bool changed=held;
    held=false;
    wipe(text,sizeof(text));
    return changed;
}

void ryz_v5_password_bind(const ryz_v5_password_binding_t *next)
{
    (void)ryz_v5_password_revoke();
    metadata=(ryz_v5_password_metadata_t){0};
    read_error=ESP_OK;
    binding=next ? *next : (ryz_v5_password_binding_t){0};
}

bool ryz_v5_password_refresh(bool eligible)
{
    const ryz_v5_password_metadata_t old=metadata;
    const esp_err_t old_error=read_error;
    ryz_v5_password_metadata_t next={0};
    esp_err_t error=ESP_OK;
    if(eligible && binding.inspect && binding.copy)
        error=binding.inspect(binding.context,&next);
    if(error!=ESP_OK || !next.available || !next.epoch)
        next=(ryz_v5_password_metadata_t){0};
    bool changed=false;
    if(held && (!next.available || next.epoch!=old.epoch))
        changed=ryz_v5_password_revoke();
    metadata=next;
    if(!eligible) read_error=ESP_OK;
    else if(error!=ESP_OK) read_error=error;
    /* A recovered provider never resumes an old hold. A new DOWN explicitly
     * retries, including a password unsupported by the current ASCII fonts. */
    return changed || old.available!=next.available || old.epoch!=next.epoch ||
        old_error!=read_error;
}

bool ryz_v5_password_can_hold(void)
{ return metadata.available && metadata.epoch && binding.copy; }
bool ryz_v5_password_held(void) { return held; }
bool ryz_v5_password_needs_cleanup(void) { return tainted; }

bool ryz_v5_password_begin(void)
{
    if(held || tainted || !ryz_v5_password_can_hold()) return false;
    char password[PASSWORD_CAPACITY]={0};
    read_error=binding.copy(binding.context,metadata.epoch,password,sizeof(password));
    const char *end=memchr(password,0,sizeof(password));
    if(read_error==ESP_OK && !end) read_error=ESP_ERR_INVALID_SIZE;
    size_t length=end ? (size_t)(end-password) : 0;
    for(size_t i=0;read_error==ESP_OK && i<length;++i) {
        unsigned char c=(unsigned char)password[i];
        if(c<32 || c>126) read_error=ESP_ERR_NOT_SUPPORTED;
    }
    if(read_error==ESP_OK) {
        /* Mark before any sensitive bytes can reach the canvas. Cleanup is
         * mandatory even if the subsequent candidate/transfer fails. */
        read_error=ryz_display_privacy_mark_sensitive();
        if(read_error==ESP_OK) {
            tainted=true;
            held=true;
            memcpy(text,password,length);
            text[length]=0;
        }
    }
    wipe(password,sizeof(password));
    return held;
}

esp_err_t ryz_v5_password_cleanup(void)
{
    (void)ryz_v5_password_revoke();
    if(!tainted) return ESP_OK;
    esp_err_t error=ryz_display_privacy_clear();
    if(error==ESP_OK) tainted=false;
    return error;
}

esp_err_t ryz_v5_password_draw(lv_obj_t *root)
{ return ryz_v5_password_draw_at(root, 64, 154, 164); }

esp_err_t ryz_v5_password_draw_at(lv_obj_t *root, int x, int y, int width)
{
    if(!root) return ESP_ERR_INVALID_ARG;
    /* The generic helper sees only an empty non-secret literal. */
    lv_obj_t *label=ryz_v5_label(root,x,y,width,20,"",RYZ_FONT_BODY,14,
                                V5_BODY,LV_TEXT_ALIGN_LEFT);
    if(!label) {
        esp_err_t error=ryz_v5_widgets_status();
        return error==ESP_OK ? ESP_ERR_NO_MEM : error;
    }
    const char *shown=held ? (text[0] ? text : "OPEN NETWORK") :
        metadata.available ? "************" : "--";
    lv_label_set_long_mode(label,held ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text_static(label,shown);
    return ESP_OK;
}

const char *ryz_v5_password_hint(void)
{
    if(held) return "RELEASE TO HIDE";
    if(read_error==ESP_ERR_NOT_SUPPORTED) return "ASCII FONT ONLY";
    if(read_error!=ESP_OK) return "HOLD AGAIN TO RETRY";
    return metadata.available ? "HOLD TO VIEW PASSWORD" : "PASSWORD UNAVAILABLE";
}
