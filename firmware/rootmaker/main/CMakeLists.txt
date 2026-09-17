set(ryz_boot_diagnostics)
if(CONFIG_RYZ_BOOT_DIAGNOSTICS)
    list(APPEND ryz_boot_diagnostics
         "system_boot_diag.lua"
         "../scripts/display_demo.lua"
         "../scripts/touch_demo.lua")
endif()

idf_component_register(
    SRCS "main.c" "ryz_json_memory.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES ${ryz_boot_diagnostics}
    REQUIRES ryz_board ryz_font ryz_workbench ryz_display_settings spiffs log heap json)
