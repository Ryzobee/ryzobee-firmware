#pragma once
#include "ryz_system_ui.h"
#include "lvgl.h"

/* Owner copies a completed, bounded fault. No VM, file or HTTP access in UI. */
typedef struct {
    char code[24], summary[96], source_file[41];
    int32_t line; /* -1 unknown */
} ryz_v5_fault_model_t;
void ryz_v5_fault_set(const ryz_v5_fault_model_t *model);
bool ryz_v5_fault_link(const char *relative_path); /* NULL revokes access */
esp_err_t ryz_v5_fault_draw(lv_obj_t *root,const ryz_system_ui_snapshot_t *system);
/* 0 none, 1 Home, 2 switch AP join/log. */
unsigned ryz_v5_fault_hit(uint16_t x,uint16_t y);
void ryz_v5_fault_next(void);
bool ryz_v5_fault_needs_cleanup(void);
esp_err_t ryz_v5_fault_cleanup(void);
