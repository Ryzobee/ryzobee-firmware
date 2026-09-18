#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

app_files()
{
  printf '%s\n' \
    components/ryz_runtime/app_runtime.c \
    components/ryz_runtime/lua_sandbox.h \
    components/ryz_runtime/lua_coroutines.h \
    components/ryz_runtime/lua_hardware.h \
    components/ryz_runtime/lua_peripherals.h \
    components/ryz_runtime/lua_fs.h \
    components/ryz_runtime/include/ryz_fs.h \
    components/ryz_app_fs/CMakeLists.txt \
    components/ryz_app_fs/ryz_app_fs.c \
    components/ryz_app_fs/ryz_app_fs_esp.c \
    components/ryz_app_fs/ryz_fs_platform.h \
    components/ryz_script_store/CMakeLists.txt \
    components/ryz_script_store/ryz_script_store.c \
    components/ryz_script_store/ryz_script_store_esp.c \
    components/ryz_script_store/ryz_script_store_platform.h \
    components/ryz_script_store/include/ryz_script_store.h \
    components/ryz_runtime/include/ryz_peripheral.h \
    components/ryz_runtime/peripheral_esp.c \
    components/ryz_log/CMakeLists.txt \
    components/ryz_log/ryz_log.c \
    components/ryz_log/include/ryz_log.h \
    components/ryz_runtime/lua_hardware_esp.c \
    components/ryz_runtime/lua_hardware_esp.h \
    components/ryz_runtime/include/ryz_lua_hardware.h \
    components/ryz_board/imu.c \
    components/ryz_board/include/imu.h \
    components/ryz_provisioning/include/ryz_provisioning.h \
    components/ryz_ble/include/ryz_ble.h \
    components/ryz_ble/ryz_ble.c \
    components/ryz_ble/ryz_ble_store.c \
    components/ryz_ble/ryz_ble_store.h \
    components/ryz_ble/ryz_ble_sdk.h \
    components/ryz_ble/ryz_ble_hid_profile.c \
    components/ryz_ble/ryz_ble_hid_profile.h \
    components/ryz_runtime/include/app_runtime.h \
    components/ryz_runtime/lua_runtime.c \
    components/ryz_runtime/include/lua_runtime.h \
    components/ryz_runtime/ryz_runtime_io.c \
    components/ryz_runtime/include/ryz_runtime_io.h \
    components/ryz_workbench/workbench_io.c \
    components/ryz_workbench/workbench_io.h \
    components/ryz_workbench/workbench_input.c \
    components/ryz_workbench/workbench_input.h \
    components/ryz_lvgl/include/ryz_ui.h \
    components/ryz_lvgl/include/ryz_lvgl.h \
    components/ryz_lvgl/include/ryz_lvgl_system.h \
    components/ryz_lvgl/ryz_lvgl.c \
    components/ryz_lvgl/ryz_lvgl_perf.c \
    components/ryz_lvgl/ryz_lvgl_perf.h \
    components/ryz_lvgl/ryz_lvgl_memory.h \
    components/ryz_lvgl/CMakeLists.txt \
    components/ryz_font/CMakeLists.txt \
    components/ryz_lvgl/include/ryz_lvgl_brand_font.h \
    components/ryz_lvgl/ryz_lvgl_brand_font.c \
    components/ryz_font/include/ryz_font.h \
    components/ryz_font/ryz_font.c \
    components/ryz_font/ryz_font_disabled.c \
    components/ryz_font/ryz_font_bitmap.c \
    components/ryz_font/ryz_font_bitmap.h \
    components/ryz_font/assets/manifest.json \
    components/ryz_font/assets/noto_sans_regular_ascii.ttf \
    components/ryz_font/assets/noto_sans_semibold_ascii.ttf \
    components/ryz_font/assets/teko_semibold_ascii.ttf \
    components/ryz_font/assets/noto_sans_medium_ascii.ttf \
    components/ryz_font/assets/roboto_mono_regular_ascii.ttf \
    components/ryz_font/assets/roboto_mono_medium_ascii.ttf \
    components/ryz_font/assets/roboto_mono_semibold_ascii.ttf \
    components/ryz_board/board_i2c.c \
    components/ryz_board/i2c_idf_compat.c \
    components/ryz_board/include/board_i2c.h \
    components/ryz_i2c_scan/include/ryz_i2c_scan.h \
    components/ryz_i2c_scan/include/ryz_i2c_scan_types.h \
    components/ryz_i2c_scan/ryz_i2c_scan.c \
    components/ryz_i2c_scan/ryz_i2c_scan_bus.c \
    components/ryz_i2c_scan/ryz_i2c_scan_bus.h \
    components/ryz_i2c_scan/ryz_i2c_scan_esp.c \
    components/ryz_i2c_scan/ryz_i2c_scan_idf.c \
    components/ryz_i2c_scan/ryz_i2c_scan_idf.h \
    components/ryz_i2c_scan/ryz_i2c_scan_platform.h \
    components/ryz_monitor/include/ryz_monitor.h \
    components/ryz_monitor/include/ryz_monitor_types.h \
    components/ryz_monitor/ryz_monitor.c \
    components/ryz_monitor/ryz_monitor_esp.c \
    components/ryz_monitor/ryz_monitor_log.c \
    components/ryz_monitor/ryz_monitor_platform.h \
    components/ryz_monitor/ryz_monitor_source.h \
    components/ryz_monitor/ryz_monitor_stream.c \
    components/ryz_monitor/ryz_monitor_stream.h \
    components/ryz_monitor/ryz_monitor_uart.c \
    components/ryz_rgb/include/ryz_rgb.h \
    components/ryz_rgb/include/ryz_rgb_types.h \
    components/ryz_rgb/ryz_rgb.c \
    components/ryz_rgb/ryz_rgb_driver.c \
    components/ryz_rgb/ryz_rgb_driver.h \
    components/ryz_rgb/ryz_rgb_esp.c \
    components/ryz_rgb/ryz_rgb_platform.h \
    components/ryz_runtime/app_tools.c \
    components/ryz_runtime/app_tools.h \
    components/ryz_tool_pins/include/ryz_tool_pins.h \
    components/ryz_tool_pins/ryz_tool_pins.c \
    components/ryz_tools/include/ryz_tool_call.h \
    components/ryz_tools/include/ryz_tools.h \
    components/ryz_tools/ryz_tools.c \
    components/ryz_system_ui/ryz_system_ui.c \
    components/ryz_system_ui/include/ryz_system_ui.h \
    components/ryz_system_ui/ryz_v5_ble.c \
    components/ryz_system_ui/include/ryz_v5_ble.h \
    components/ryz_system_ui/ryz_v5_apps.c \
    components/ryz_system_ui/include/ryz_v5_apps.h \
    components/ryz_system_ui/ryz_v5_render.c \
    components/ryz_system_ui/ryz_v5_widgets.c \
    components/ryz_system_ui/include/ryz_v5_widgets.h \
    components/ryz_system_ui/ryz_v5_font.c \
    components/ryz_system_ui/include/ryz_v5_font.h \
    components/ryz_system_ui/ryz_v5_font_data.inc \
    components/ryz_system_ui/ryz_v5_boot.c \
    components/ryz_system_ui/include/ryz_v5_boot.h \
    components/ryz_system_ui/ryz_v5_boot_asset.c \
    components/ryz_system_ui/ryz_v5_font_palette.def \
    components/ryz_system_ui/ryz_v5_status.c \
    components/ryz_system_ui/include/ryz_v5_status.h \
    components/ryz_system_ui/ryz_v5_render_dirty.c \
    components/ryz_system_ui/include/ryz_v5_render_dirty.h \
    components/ryz_system_ui/CMakeLists.txt \
    components/ryz_workbench/workbench.c \
    components/ryz_workbench/workbench_ble.c \
    components/ryz_workbench/workbench_ble.h \
    components/ryz_workbench/workbench_boot.c \
    components/ryz_workbench/workbench_boot.h \
    components/ryz_workbench/workbench_job_start.c \
    components/ryz_workbench/workbench_job_start.h \
    components/ryz_workbench/workbench_tools.c \
    components/ryz_workbench/workbench_tools.h
  find managed_components/georgik__lua/include -type f -maxdepth 1 -name '*.h' -print
  find managed_components/georgik__lua/lua -type f -maxdepth 1 \( -name '*.h' -o -name '*.c' \) -print
}

{
  app_files | LC_ALL=C sort | while IFS= read -r file; do
    digest=$(shasum -a 256 "$file" | awk '{print $1}')
    printf '%s:%s\n' "$file" "$digest"
  done
  printf 'CONFIG_LUA_MAXSTACK=8192\n'
  # Supported Studio Host profile: static Lua fonts. An opt-in dynamic-font
  # firmware intentionally has another fingerprint and needs matching proof.
  printf 'CONFIG_RYZ_LUA_FREETYPE=\n'
  printf 'LVGL_VERSION=9.5.0\n'
  printf 'LVGL_SYSMON=y,y,y,\n'
  printf 'LVGL_CONFIG=y,16,64,y,4,1,1,y,y,y,y,y,y,y,y,\n'
} | shasum -a 256 | awk '{print $1}'
