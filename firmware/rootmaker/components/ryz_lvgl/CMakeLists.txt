set(ryz_lua_font_sources)
if(CONFIG_RYZ_LUA_FREETYPE)
    list(APPEND ryz_lua_font_sources "ryz_lvgl_brand_font.c")
endif()
idf_component_register(
    SRCS "ryz_lvgl.c" ${ryz_lua_font_sources} "ryz_lvgl_perf.c" "ryz_lua_assets.c"
         "../ryz_system_ui/ryz_v5_font.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../ryz_system_ui/include"
    REQUIRES ryz_board ryz_font lvgl esp_timer esp_system
    PRIV_REQUIRES freertos heap)

# Keep LVGL's bounded builtin TLSF allocator and its configured capacity, but
# source its boot-lifetime pool from PSRAM instead of a 64 KiB internal .bss.
# Host pixels retain their ordinary static pool; this is target heap routing.
idf_component_get_property(ryz_lvgl_library lvgl__lvgl COMPONENT_LIB)
target_include_directories(${ryz_lvgl_library} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(${ryz_lvgl_library} PRIVATE
    LV_MEM_POOL_INCLUDE="ryz_lvgl_memory.h"
    LV_MEM_POOL_ALLOC=ryz_lvgl_memory_pool)
