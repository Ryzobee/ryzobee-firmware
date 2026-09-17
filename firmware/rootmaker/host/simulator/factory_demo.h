#pragma once
#include "app_runtime.h"

typedef enum {
    RYZ_SIM_FACTORY_NONE = 0,
    RYZ_SIM_FACTORY_MONITOR,
    RYZ_SIM_FACTORY_I2C,
    RYZ_SIM_FACTORY_HARDWARE,
} ryz_sim_factory_kind_t;
typedef struct {
    ryz_sim_factory_kind_t kind;
    const char *name, *source, *version, *sha256;
    size_t bytes;
} ryz_sim_factory_source_t;

/* Desktop-only environment for the exact embedded factory scripts. All
 * operations stay on the SDL owner thread; no serial/network/GPIO access.
 * Synthetic drivers expose handles/samples, never a tool UI/state machine. */
const ryz_sim_factory_source_t *ryz_sim_factory_source(ryz_sim_factory_kind_t kind);
const ryz_sim_factory_source_t *ryz_sim_factory_find(const char *name);
ryz_sim_factory_kind_t ryz_sim_factory_kind(void);
void ryz_sim_factory_run(ryz_sim_factory_kind_t kind,void (*idle)(unsigned),
                         void (*present)(void),ryz_lua_result_t *result);
bool ryz_sim_factory_active(void);
void ryz_sim_factory_cancel(void);
void ryz_sim_factory_pointer(const ryz_app_pointer_t *sample);
const ryz_ui_scene_t *ryz_sim_factory_scene(void);
