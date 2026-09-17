#include "workbench_version.h"
#include <stdio.h>

static const char *image_state(const ryz_ota_running_info_t *info)
{
    if(!info->state_known || info->state_error!=ESP_OK) return "--";
    switch(info->state) {
    case RYZ_OTA_IMAGE_STATE_NEW: return "NEW";
    case RYZ_OTA_IMAGE_STATE_PENDING_VERIFY: return "PENDING";
    case RYZ_OTA_IMAGE_STATE_VALID: return "VALID";
    case RYZ_OTA_IMAGE_STATE_INVALID: return "INVALID";
    case RYZ_OTA_IMAGE_STATE_ABORTED: return "ABORTED";
    case RYZ_OTA_IMAGE_STATE_UNDEFINED: return "UNDEFINED";
    default: return "--";
    }
}

static bool displayable_label(const char label[17])
{
    for(size_t i=0;i<17;++i) {
        const unsigned char c=(unsigned char)label[i];
        if(!c) return i!=0;
        if(c<0x20 || c>0x7e) return false;
    }
    return false;
}

static void checked_format_result(char *text,size_t capacity,int written)
{
    /* Preserve unknown instead of a plausible but truncated identity. */
    if(written<0 || (size_t)written>=capacity) text[0]='\0';
}

void ryz_workbench_version_hardware(ryz_v5_version_model_t *view,
                                   const ryz_ota_running_info_t *info,
                                   bool is_esp32s3, uint16_t chip_revision)
{
    if(!view) return;
    view->slot[0]='\0';
    view->chip[0]='\0';
    view->ab_slots=false;
    if(is_esp32s3) {
        int n=snprintf(view->chip,sizeof(view->chip),"ESP32-S3 REV %u.%u",
            (unsigned)chip_revision/100U,(unsigned)chip_revision%100U);
        checked_format_result(view->chip,sizeof(view->chip),n);
    }
    if(!info) return;
    view->ab_slots=info->ab_slots_known && info->ab_slots;
    if(!info->partition_known) return;
    char fallback[17];
    const char *label=info->running_label;
    if(!displayable_label(info->running_label)) {
        /* Actual app subtype, not a guess from CONFIG or selected boot slot.
         * OTA subtypes are 0x10..0x1f in the locked ESP-IDF partition ABI. */
        if(info->running_subtype>=0x10 && info->running_subtype<=0x1f)
            (void)snprintf(fallback,sizeof(fallback),"OTA_%u",
                (unsigned)info->running_subtype-0x10U);
        else (void)snprintf(fallback,sizeof(fallback),"%s",
            info->running_subtype==0?"FACTORY":info->running_subtype==0x20?"TEST":"APP");
        label=fallback;
    }
    int n=snprintf(view->slot,sizeof(view->slot),"%s / %s",label,image_state(info));
    checked_format_result(view->slot,sizeof(view->slot),n);
}
