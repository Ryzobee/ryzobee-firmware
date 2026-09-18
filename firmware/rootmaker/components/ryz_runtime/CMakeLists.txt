idf_component_get_property(ryz_peripheral_i2c_dir esp_driver_i2c COMPONENT_DIR)
idf_component_register(
    SRCS "lua_runtime.c" "app_runtime.c" "app_tools.c" "nervous_runtime.c" "ryz_runtime_io.c" "lua_hardware_esp.c" "peripheral_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${ryz_peripheral_i2c_dir}"
    REQUIRES ryz_board ryz_lvgl ryz_tools georgik__lua esp_timer esp_psram spi_flash esp_system
    PRIV_REQUIRES ryz_provisioning ryz_ble ryz_tool_pins ryz_log ryz_rgb ryz_app_fs
        esp_driver_gpio esp_driver_uart esp_driver_i2c esp_driver_spi
        esp_driver_ledc esp_adc esp_hw_support log freertos)

# Deployment compares these common-runtime/Lua/config fingerprints with the
# simulator. The authoritative source paths are intentionally part of the hash
# material. Moving from main/ to components/ therefore invalidates every old
# receipt even when the source bytes are unchanged.
set(runtime_root "${CMAKE_CURRENT_LIST_DIR}/../..")
file(GLOB runtime_lua_files RELATIVE "${runtime_root}"
    "${runtime_root}/managed_components/georgik__lua/include/*.h"
    "${runtime_root}/managed_components/georgik__lua/lua/*.h"
    "${runtime_root}/managed_components/georgik__lua/lua/*.c")

function(ryz_runtime_hash output_variable)
    set(runtime_files ${ARGN} ${runtime_lua_files})
    list(SORT runtime_files)
    set(runtime_material "")
    foreach(runtime_file IN LISTS runtime_files)
        file(SHA256 "${runtime_root}/${runtime_file}" runtime_file_sha)
        string(APPEND runtime_material "${runtime_file}:${runtime_file_sha}\n")
        # CMake already tracks included component CMakeLists. Adding their
        # ../.. aliases again emits duplicate Ninja reconfiguration outputs.
        if(NOT runtime_file MATCHES "/CMakeLists[.]txt$")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${runtime_root}/${runtime_file}")
        endif()
    endforeach()
    string(APPEND runtime_material "CONFIG_LUA_MAXSTACK=${CONFIG_LUA_MAXSTACK}\n")
    # LVGL belongs to the app renderer contract, not the neuro core. Match the
    # Host's existing neuro source/Lua/MAXSTACK contract without expanding it.
    if(output_variable STREQUAL "app_core_sha")
        string(APPEND runtime_material "CONFIG_RYZ_LUA_FREETYPE=${CONFIG_RYZ_LUA_FREETYPE}\n")
        string(APPEND runtime_material "LVGL_VERSION=9.5.0\n")
        string(APPEND runtime_material
            "LVGL_SYSMON=${CONFIG_LV_USE_OBSERVER},${CONFIG_LV_USE_SYSMON},${CONFIG_LV_USE_PERF_MONITOR},${CONFIG_LV_USE_PERF_MONITOR_LOG_MODE}\n")
        string(APPEND runtime_material
            "LVGL_CONFIG=${CONFIG_LV_CONF_MINIMAL},${CONFIG_LV_COLOR_DEPTH},${CONFIG_LV_MEM_SIZE_KILOBYTES},${CONFIG_LV_OS_NONE},${CONFIG_LV_DRAW_BUF_ALIGN},${CONFIG_LV_DRAW_BUF_STRIDE_ALIGN},${CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT},${CONFIG_LV_DRAW_SW_SUPPORT_RGB565},${CONFIG_LV_DRAW_SW_COMPLEX},${CONFIG_LV_USE_LABEL},${CONFIG_LV_FONT_MONTSERRAT_16},${CONFIG_LV_FONT_MONTSERRAT_24},${CONFIG_LV_USE_BUILTIN_MALLOC},${CONFIG_LV_USE_IMAGE},${CONFIG_LV_TXT_ENC_UTF8},${CONFIG_LV_TXT_ENC_ASCII}\n")
    endif()
    string(SHA256 runtime_sha "${runtime_material}")
    set(${output_variable} "${runtime_sha}" PARENT_SCOPE)
endfunction()

ryz_runtime_hash(neuro_core_sha
    components/ryz_runtime/nervous_runtime.c
    components/ryz_runtime/include/nervous_runtime.h
    components/ryz_runtime/include/lua_runtime.h)

# The generic application runtime hash includes the ESP execution Adapter and
# frozen Lua VM/configuration because all of them affect real-device semantics.
ryz_runtime_hash(app_core_sha
    components/ryz_runtime/app_runtime.c
    components/ryz_runtime/lua_sandbox.h
    components/ryz_runtime/lua_coroutines.h
    components/ryz_runtime/lua_hardware.h
    components/ryz_runtime/lua_peripherals.h
    components/ryz_runtime/lua_fs.h
    components/ryz_runtime/include/ryz_fs.h
    components/ryz_runtime/lua_boot.h
    components/ryz_runtime/include/ryz_lua_boot.h
    components/ryz_workbench/workbench_boot_events.c
    components/ryz_workbench/include/workbench_boot_events.h
    components/ryz_workbench/workbench_boot_key.c
    components/ryz_workbench/include/workbench_boot_key.h
    components/ryz_board/boot_button.c
    components/ryz_board/include/boot_button.h
    components/ryz_app_fs/CMakeLists.txt
    components/ryz_app_fs/ryz_app_fs.c
    components/ryz_app_fs/ryz_app_fs_esp.c
    components/ryz_app_fs/ryz_fs_platform.h
    components/ryz_script_store/CMakeLists.txt
    components/ryz_script_store/ryz_script_store.c
    components/ryz_script_store/ryz_script_store_esp.c
    components/ryz_script_store/ryz_script_store_platform.h
    components/ryz_script_store/include/ryz_script_store.h
    components/ryz_runtime/include/ryz_peripheral.h
    components/ryz_runtime/peripheral_esp.c
    components/ryz_log/CMakeLists.txt
    components/ryz_log/ryz_log.c
    components/ryz_log/include/ryz_log.h
    components/ryz_runtime/lua_hardware_esp.c
    components/ryz_runtime/lua_hardware_esp.h
    components/ryz_runtime/include/ryz_lua_hardware.h
    components/ryz_board/imu.c
    components/ryz_board/include/imu.h
    components/ryz_provisioning/include/ryz_provisioning.h
    components/ryz_ble/include/ryz_ble.h
    components/ryz_ble/ryz_ble.c
    components/ryz_ble/ryz_ble_store.c
    components/ryz_ble/ryz_ble_store.h
    components/ryz_ble/ryz_ble_sdk.h
    components/ryz_ble/ryz_ble_hid_profile.c
    components/ryz_ble/ryz_ble_hid_profile.h
    components/ryz_runtime/include/app_runtime.h
    components/ryz_runtime/lua_runtime.c
    components/ryz_runtime/include/lua_runtime.h
    components/ryz_runtime/ryz_runtime_io.c
    components/ryz_runtime/include/ryz_runtime_io.h
    components/ryz_workbench/workbench_io.c
    components/ryz_workbench/workbench_io.h
    components/ryz_workbench/workbench_input.c
    components/ryz_workbench/workbench_input.h
    components/ryz_lvgl/include/ryz_ui.h
    components/ryz_lvgl/include/ryz_lvgl.h
    components/ryz_lvgl/include/ryz_lvgl_system.h
    components/ryz_lvgl/ryz_lvgl.c
    components/ryz_lvgl/ryz_lvgl_perf.c
    components/ryz_lvgl/ryz_lvgl_perf.h
    components/ryz_lvgl/ryz_lvgl_memory.h
    components/ryz_lvgl/CMakeLists.txt
    components/ryz_font/CMakeLists.txt
    components/ryz_lvgl/include/ryz_lvgl_brand_font.h
    components/ryz_lvgl/ryz_lvgl_brand_font.c
    components/ryz_font/include/ryz_font.h
    components/ryz_font/ryz_font.c
    components/ryz_font/ryz_font_disabled.c
    components/ryz_font/ryz_font_bitmap.c
    components/ryz_font/ryz_font_bitmap.h
    components/ryz_font/assets/manifest.json
    components/ryz_font/assets/noto_sans_regular_ascii.ttf
    components/ryz_font/assets/noto_sans_semibold_ascii.ttf
    components/ryz_font/assets/teko_semibold_ascii.ttf
    components/ryz_font/assets/noto_sans_medium_ascii.ttf
    components/ryz_font/assets/roboto_mono_regular_ascii.ttf
    components/ryz_font/assets/roboto_mono_medium_ascii.ttf
    components/ryz_font/assets/roboto_mono_semibold_ascii.ttf
    components/ryz_board/board_i2c.c
    components/ryz_board/i2c_idf_compat.c
    components/ryz_board/include/board_i2c.h
    components/ryz_i2c_scan/include/ryz_i2c_scan.h
    components/ryz_i2c_scan/include/ryz_i2c_scan_types.h
    components/ryz_i2c_scan/ryz_i2c_scan.c
    components/ryz_i2c_scan/ryz_i2c_scan_bus.c
    components/ryz_i2c_scan/ryz_i2c_scan_bus.h
    components/ryz_i2c_scan/ryz_i2c_scan_esp.c
    components/ryz_i2c_scan/ryz_i2c_scan_idf.c
    components/ryz_i2c_scan/ryz_i2c_scan_idf.h
    components/ryz_i2c_scan/ryz_i2c_scan_platform.h
    components/ryz_monitor/include/ryz_monitor.h
    components/ryz_monitor/include/ryz_monitor_types.h
    components/ryz_monitor/ryz_monitor.c
    components/ryz_monitor/ryz_monitor_esp.c
    components/ryz_monitor/ryz_monitor_log.c
    components/ryz_monitor/ryz_monitor_platform.h
    components/ryz_monitor/ryz_monitor_source.h
    components/ryz_monitor/ryz_monitor_stream.c
    components/ryz_monitor/ryz_monitor_stream.h
    components/ryz_monitor/ryz_monitor_uart.c
    components/ryz_rgb/include/ryz_rgb.h
    components/ryz_rgb/include/ryz_rgb_types.h
    components/ryz_rgb/ryz_rgb.c
    components/ryz_rgb/ryz_rgb_driver.c
    components/ryz_rgb/ryz_rgb_driver.h
    components/ryz_rgb/ryz_rgb_esp.c
    components/ryz_rgb/ryz_rgb_platform.h
    components/ryz_runtime/app_tools.c
    components/ryz_runtime/app_tools.h
    components/ryz_tool_pins/include/ryz_tool_pins.h
    components/ryz_tool_pins/ryz_tool_pins.c
    components/ryz_tools/include/ryz_tool_call.h
    components/ryz_tools/include/ryz_tools.h
    components/ryz_tools/ryz_tools.c
    components/ryz_system_ui/ryz_system_ui.c
    components/ryz_system_ui/include/ryz_system_ui.h
    components/ryz_system_ui/ryz_v5_ble.c
    components/ryz_system_ui/include/ryz_v5_ble.h
    components/ryz_system_ui/ryz_v5_apps.c
    components/ryz_system_ui/include/ryz_v5_apps.h
    components/ryz_system_ui/ryz_v5_render.c
    components/ryz_system_ui/ryz_v5_widgets.c
    components/ryz_system_ui/include/ryz_v5_widgets.h
    components/ryz_system_ui/ryz_v5_font.c
    components/ryz_system_ui/include/ryz_v5_font.h
    components/ryz_system_ui/ryz_v5_font_data.inc
    components/ryz_system_ui/ryz_v5_boot.c
    components/ryz_system_ui/include/ryz_v5_boot.h
    components/ryz_system_ui/ryz_v5_boot_asset.c
    components/ryz_system_ui/ryz_v5_font_palette.def
    components/ryz_system_ui/ryz_v5_status.c
    components/ryz_system_ui/include/ryz_v5_status.h
    components/ryz_system_ui/ryz_v5_render_dirty.c
    components/ryz_system_ui/include/ryz_v5_render_dirty.h
    components/ryz_system_ui/CMakeLists.txt
    components/ryz_workbench/workbench.c
    components/ryz_workbench/workbench_ble.c
    components/ryz_workbench/workbench_ble.h
    components/ryz_workbench/workbench_boot.c
    components/ryz_workbench/workbench_boot.h
    components/ryz_workbench/workbench_job_start.c
    components/ryz_workbench/workbench_job_start.h
    components/ryz_workbench/workbench_tools.c
    components/ryz_workbench/workbench_tools.h)

# main and ryz_workbench report these fingerprints, so the definitions are part
# of the module interface rather than private implementation-only definitions.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    RYZ_NEURO_CORE_SHA="${neuro_core_sha}"
    RYZ_APP_CORE_SHA="${app_core_sha}")
