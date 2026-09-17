#pragma once

#include "app_runtime.h"

/* Desktop-only adapter for the embedded factory Monitor script. No serial
 * ports, networking, filesystem mutation or general hardware emulation.
 * All callbacks and Lua/LVGL calls stay on the SDL owner thread. */
void ryz_sim_monitor_run(void (*idle)(unsigned ms), void (*present)(void),
                         ryz_lua_result_t *result);
bool ryz_sim_monitor_active(void);
void ryz_sim_monitor_cancel(void);
void ryz_sim_monitor_pointer(const ryz_app_pointer_t *sample);
size_t ryz_sim_monitor_source_bytes(void);
const char *ryz_sim_monitor_version(void);
/* Read-only observation of the real Lua scene, for SDL event regression. */
const ryz_ui_scene_t *ryz_sim_monitor_scene(void);
