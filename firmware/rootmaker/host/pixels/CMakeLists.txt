cmake_minimum_required(VERSION 3.20)
project(ryz_pixels_host LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(RYZ_FIRMWARE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# Keep the release version in the ESP-IDF project as the single source of
# truth. Host pages use that version while retaining explicit fixture data.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RYZ_FIRMWARE}/CMakeLists.txt")
file(STRINGS "${RYZ_FIRMWARE}/CMakeLists.txt" RYZ_PROJECT_VERSION_LINE
    REGEX "^set\\(PROJECT_VER \"[0-9]+\\.[0-9]+\\.[0-9]+\"\\)$")
list(LENGTH RYZ_PROJECT_VERSION_LINE RYZ_PROJECT_VERSION_COUNT)
if(NOT RYZ_PROJECT_VERSION_COUNT EQUAL 1)
    message(FATAL_ERROR "The firmware project must declare one numeric PROJECT_VER")
endif()
string(REGEX REPLACE "^set\\(PROJECT_VER \"([^\"]+)\"\\)$" "\\1"
    RYZ_HOST_FIRMWARE_VERSION "${RYZ_PROJECT_VERSION_LINE}")
set(RYZ_IDF_BUILD "" CACHE PATH "Existing ESP-IDF target build containing config/sdkconfig.h")
if(NOT EXISTS "${RYZ_IDF_BUILD}/config/sdkconfig.h")
    message(FATAL_ERROR "Pass -DRYZ_IDF_BUILD=/absolute/path/to/a/current/target/build")
endif()
file(REAL_PATH "${RYZ_IDF_BUILD}" RYZ_IDF_BUILD)
set(RYZ_LVGL "${RYZ_FIRMWARE}/managed_components/lvgl__lvgl")
set(RYZ_FREETYPE "${RYZ_FIRMWARE}/managed_components/espressif__freetype/freetype")
if(NOT EXISTS "${RYZ_LVGL}/CMakeLists.txt" OR NOT EXISTS "${RYZ_FREETYPE}/CMakeLists.txt")
    message(FATAL_ERROR "Locked managed components are missing; configure the ESP-IDF target first")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "This diagnostic requires a Clang/GCC sanitizer toolchain")
endif()
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=address,undefined)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
# Match the target's disabled optional image/compression dependencies. Do not
# silently pick up Homebrew/system libraries unavailable on the MCU.
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_PNG TRUE)
set(SKIP_INSTALL_ALL TRUE)
add_subdirectory("${RYZ_FREETYPE}" freetype)
target_compile_options(freetype PRIVATE -Wno-unused-but-set-variable)

configure_file(lv_conf.h.in "${CMAKE_CURRENT_BINARY_DIR}/lv_conf.h" @ONLY)
configure_file(fonts.S.in "${CMAKE_CURRENT_BINARY_DIR}/fonts.S" @ONLY)
# .incbin is not tracked by every assembler dependency scanner. Regenerated
# subsets must rebuild the embedded bytes, even in an existing Host tree.
set(RYZ_EMBEDDED_FONT_FILES)
foreach(font IN ITEMS noto_sans_regular_ascii.ttf noto_sans_semibold_ascii.ttf
        teko_semibold_ascii.ttf noto_sans_medium_ascii.ttf
        roboto_mono_regular_ascii.ttf roboto_mono_medium_ascii.ttf
        roboto_mono_semibold_ascii.ttf)
    list(APPEND RYZ_EMBEDDED_FONT_FILES "${RYZ_FIRMWARE}/components/ryz_font/assets/${font}")
endforeach()
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/fonts.S" PROPERTIES
    OBJECT_DEPENDS "${RYZ_EMBEDDED_FONT_FILES}")
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_BINARY_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_USE_KCONFIG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${RYZ_LVGL}" lvgl)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Only OS/heap/flush are Host adapters. The font renderer, bitmap copier,
# LVGL font adapter, libraries and embedded font bytes are production sources.
add_library(ryz_font_bitmap STATIC
    "${RYZ_FIRMWARE}/components/ryz_font/ryz_font_bitmap.c")
target_compile_options(ryz_font_bitmap PRIVATE -Wall -Wextra -Werror)
add_library(ryz_font_offline STATIC
    "${RYZ_FIRMWARE}/components/ryz_font/ryz_font.c"
    "${RYZ_FIRMWARE}/components/ryz_lvgl/ryz_lvgl_brand_font.c"
    "${CMAKE_CURRENT_BINARY_DIR}/fonts.S"
    platform.c)
target_include_directories(ryz_font_offline PUBLIC
    stubs
    "${RYZ_FIRMWARE}/components/ryz_font/include"
    "${RYZ_FIRMWARE}/components/ryz_lvgl/include")
target_compile_options(ryz_font_offline PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ryz_font_offline PUBLIC lvgl freetype Threads::Threads ryz_font_bitmap)
# The offline oracle/exporter retains FreeType. Runtime and SDL select the
# actual target backend and must not pull that oracle into a static build.
file(STRINGS "${RYZ_IDF_BUILD}/config/sdkconfig.h" RYZ_FT_ENABLED
    REGEX "^#define CONFIG_RYZ_LUA_FREETYPE 1$")
if(RYZ_FT_ENABLED)
    add_library(ryz_brand_font ALIAS ryz_font_offline)
else()
    add_library(ryz_brand_font STATIC
        "${RYZ_FIRMWARE}/components/ryz_font/ryz_font_disabled.c" platform.c)
    target_include_directories(ryz_brand_font PUBLIC stubs
        "${RYZ_FIRMWARE}/components/ryz_font/include"
        "${RYZ_FIRMWARE}/components/ryz_lvgl/include")
    target_link_libraries(ryz_brand_font PUBLIC lvgl Threads::Threads)
endif()
add_library(ryz_static_font STATIC
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_font.c")
target_include_directories(ryz_static_font PUBLIC stubs
    "${RYZ_FIRMWARE}/components/ryz_font/include"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/include")
target_link_libraries(ryz_static_font PUBLIC lvgl)
add_executable(brand_font_test "${RYZ_FIRMWARE}/tests/lvgl_brand_font_test.c")
target_compile_options(brand_font_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(brand_font_test PRIVATE ryz_font_offline)

# Explicit offline asset generation; never runs on the MCU or at UI startup.
add_executable(export_v5_fonts export_v5_fonts.c)
target_include_directories(export_v5_fonts PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_system_ui")
target_compile_options(export_v5_fonts PRIVATE -Wall -Wextra -Werror)
target_link_libraries(export_v5_fonts PRIVATE ryz_font_offline)

# Offline Unicode asset feasibility check. Not linked into firmware and not a
# substitute for the production font adapter or V5 layout/panel verification.
add_executable(unicode_font_probe "${RYZ_FIRMWARE}/tests/unicode_font_probe.c")
target_include_directories(unicode_font_probe PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_font")
target_compile_options(unicode_font_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(unicode_font_probe PRIVATE freetype ryz_font_bitmap)

# Exercise the exact production lease/scene bridge. The framebuffer adapter
# below stands in for the panel, not LVGL or its object/allocator lifecycle.
add_library(ryz_lvgl_bridge STATIC
    "${RYZ_FIRMWARE}/components/ryz_lvgl/ryz_lvgl.c"
    "${RYZ_FIRMWARE}/components/ryz_lvgl/ryz_lua_assets.c"
    "${RYZ_FIRMWARE}/components/ryz_lvgl/ryz_lvgl_perf.c"
    display.c)
target_include_directories(ryz_lvgl_bridge PUBLIC
    "${RYZ_FIRMWARE}/components/ryz_board/include")
target_compile_options(ryz_lvgl_bridge PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ryz_lvgl_bridge PUBLIC ryz_brand_font ryz_static_font)
add_executable(lvgl_lifecycle_test "${RYZ_FIRMWARE}/tests/lvgl_lifecycle_test.c")
target_compile_options(lvgl_lifecycle_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lvgl_lifecycle_test PRIVATE ryz_lvgl_bridge)
add_executable(lvgl_fps_test "${RYZ_FIRMWARE}/tests/lvgl_fps_test.c")
target_compile_options(lvgl_fps_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lvgl_fps_test PRIVATE ryz_lvgl_bridge)

# One bounded same-source Lua diagnostic; not a replacement simulator or a
# hardware-tools model. The actual app facade owns all scene/event semantics.
add_executable(ui_update_pixels
    ui_update_runner.c
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_runtime.c"
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_tools.c"
    "${RYZ_FIRMWARE}/managed_components/georgik__lua/lua/onelua.c")
target_compile_definitions(ui_update_pixels PRIVATE MAKE_LIB)
target_compile_options(ui_update_pixels PRIVATE -Wall -Wextra -Werror
    -include "${RYZ_FIRMWARE}/host/sdkconfig.h")
target_include_directories(ui_update_pixels PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_runtime/include"
    "${RYZ_FIRMWARE}/components/ryz_tools/include"
    "${RYZ_FIRMWARE}/components/ryz_i2c_scan/include"
    "${RYZ_FIRMWARE}/components/ryz_rgb/include"
    "${RYZ_FIRMWARE}/components/ryz_monitor/include"
    "${RYZ_FIRMWARE}/managed_components/georgik__lua/include"
    "${RYZ_FIRMWARE}/managed_components/georgik__lua/lua")
target_link_libraries(ui_update_pixels PRIVATE ryz_lvgl_bridge m)

# Factory Monitor: public Lua UART/log entrypoint and actual LVGL, byte fixture only.
add_executable(monitor_script_pixels monitor_script_runner.c
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_runtime.c"
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_tools.c"
    "${RYZ_FIRMWARE}/managed_components/georgik__lua/lua/onelua.c")
target_compile_definitions(monitor_script_pixels PRIVATE MAKE_LIB)
target_compile_options(monitor_script_pixels PRIVATE -Wall -Wextra -Werror
    -include "${RYZ_FIRMWARE}/host/sdkconfig.h")
get_target_property(RYZ_LUA_PIXEL_INCLUDES ui_update_pixels INCLUDE_DIRECTORIES)
target_include_directories(monitor_script_pixels PRIVATE ${RYZ_LUA_PIXEL_INCLUDES}
    "${RYZ_FIRMWARE}/components/ryz_monitor")
target_link_libraries(monitor_script_pixels PRIVATE ryz_lvgl_bridge m)

# Factory I2C Tools: actual Lua owns scans; the fixture supplies only bus replies.
add_executable(i2c_script_pixels i2c_script_runner.c
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_runtime.c"
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_tools.c"
    "${RYZ_FIRMWARE}/managed_components/georgik__lua/lua/onelua.c")
target_compile_definitions(i2c_script_pixels PRIVATE MAKE_LIB)
target_compile_options(i2c_script_pixels PRIVATE -Wall -Wextra -Werror
    -include "${RYZ_FIRMWARE}/host/sdkconfig.h")
target_include_directories(i2c_script_pixels PRIVATE ${RYZ_LUA_PIXEL_INCLUDES})
target_link_libraries(i2c_script_pixels PRIVATE ryz_lvgl_bridge m)

# Device Test: ordinary Lua owns test policy; only sensor/frame inputs are fixtures.
add_executable(hardware_script_pixels hardware_script_runner.c
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_runtime.c"
    "${RYZ_FIRMWARE}/components/ryz_runtime/app_tools.c"
    "${RYZ_FIRMWARE}/managed_components/georgik__lua/lua/onelua.c")
target_compile_definitions(hardware_script_pixels PRIVATE MAKE_LIB)
target_compile_options(hardware_script_pixels PRIVATE -Wall -Wextra -Werror
    -include "${RYZ_FIRMWARE}/host/sdkconfig.h")
target_include_directories(hardware_script_pixels PRIVATE ${RYZ_LUA_PIXEL_INCLUDES})
target_link_libraries(hardware_script_pixels PRIVATE ryz_lvgl_bridge m)
foreach(case IN ITEMS hub manual touch imu imu-errors imu-still imu-cancel
    rgb rgb-errors rgb-cancel rgb-skip summary unavailable)
    add_test(NAME hardware_script_${case} COMMAND hardware_script_pixels
        "${RYZ_FIRMWARE}/fs/tool_hardware.lua" ${case} "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

# Product C pages, not a Lua scene or a bitmap preview. The same page sources,
# embedded Figma masks and fonts are compiled into the ESP32-S3 firmware.
add_library(ryz_v5_native STATIC
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_system_ui.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_ui_navigation.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_render.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_widgets.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_assets.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_boot.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_boot_asset.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_fault.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_apps.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_display.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_network.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_password.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_ble.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_scripts.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_status.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_render_dirty.c"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_v5_version.c")
target_include_directories(ryz_v5_native PUBLIC
    "${RYZ_FIRMWARE}/components/ryz_system_ui"
    "${RYZ_FIRMWARE}/components/ryz_system_ui/include"
    "${RYZ_FIRMWARE}/components/ryz_apps/include"
    "${RYZ_FIRMWARE}/components/ryz_provisioning/include"
    "${RYZ_FIRMWARE}/components/ryz_display_settings/include"
    "${RYZ_FIRMWARE}/components/ryz_script_metadata/include"
    "${RYZ_FIRMWARE}/components/ryz_script_store/include")
target_compile_options(ryz_v5_native PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(ryz_v5_native PRIVATE RYZ_SYSTEM_UI_LVGL=1)
target_compile_definitions(ryz_v5_native PUBLIC
    RYZ_HOST_FIRMWARE_VERSION="${RYZ_HOST_FIRMWARE_VERSION}")
target_link_libraries(ryz_v5_native PUBLIC ryz_lvgl_bridge)
add_executable(v5_pixels v5_runner.c)
target_compile_options(v5_pixels PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_pixels PRIVATE ryz_v5_native)
add_library(ryz_v5_qr STATIC qr.c
    "${RYZ_FIRMWARE}/components/ryz_system_ui/ryz_qr_encoder_esp.c"
    "${RYZ_FIRMWARE}/managed_components/espressif__qrcode/qrcodegen.c"
    "${RYZ_FIRMWARE}/managed_components/espressif__qrcode/esp_qrcode_main.c"
    "${RYZ_FIRMWARE}/managed_components/espressif__qrcode/esp_qrcode_wrapper.c")
target_include_directories(ryz_v5_qr PRIVATE stubs
    "${RYZ_FIRMWARE}/components/ryz_system_ui/include"
    "${RYZ_FIRMWARE}/managed_components/espressif__qrcode/include")
target_compile_definitions(ryz_v5_qr PRIVATE CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=1)
target_compile_options(ryz_v5_qr PRIVATE "SHELL:-include stdlib.h" "SHELL:-include stdbool.h")
target_link_libraries(v5_pixels PRIVATE ryz_v5_qr)

add_executable(v5_fault_test "${RYZ_FIRMWARE}/tests/v5_fault_test.c"
    "${RYZ_FIRMWARE}/components/ryz_workbench/workbench_fault.c")
target_include_directories(v5_fault_test PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_workbench"
    "${RYZ_FIRMWARE}/components/ryz_runtime/include"
    "${RYZ_FIRMWARE}/components/ryz_diagnostics/include")
target_compile_options(v5_fault_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_fault_test PRIVATE ryz_v5_native ryz_v5_qr)
add_test(NAME v5_fault COMMAND v5_fault_test "${CMAKE_CURRENT_BINARY_DIR}")

# SDL is only the desktop panel/input adapter. Keep the exact product LVGL,
# font configuration, UI owner and RGB565 transfer path above.
option(RYZ_BUILD_SDL_SIMULATOR "Build interactive same-source desktop UI" OFF)
if(RYZ_BUILD_SDL_SIMULATOR)
    find_package(SDL2 REQUIRED)
    # All entries embed the exact editable factory bytes. Any source edit
    # regenerates its version/hash and the app, never an independent UI copy.
    foreach(RYZ_SIM_FACTORY_TAG IN ITEMS monitor i2c hardware)
        set(RYZ_SIM_FACTORY_SOURCE "${RYZ_FIRMWARE}/fs/tool_${RYZ_SIM_FACTORY_TAG}.lua")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RYZ_SIM_FACTORY_SOURCE}")
        file(READ "${RYZ_SIM_FACTORY_SOURCE}" RYZ_SIM_FACTORY_HEX HEX)
        file(SHA256 "${RYZ_SIM_FACTORY_SOURCE}" RYZ_SIM_FACTORY_SHA256)
        file(STRINGS "${RYZ_SIM_FACTORY_SOURCE}" RYZ_SIM_FACTORY_VERSION REGEX "^-- @version: ")
        string(REGEX REPLACE "^-- @version: " "" RYZ_SIM_FACTORY_VERSION "${RYZ_SIM_FACTORY_VERSION}")
        if(NOT RYZ_SIM_FACTORY_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
            message(FATAL_ERROR "Factory ${RYZ_SIM_FACTORY_TAG} needs an unambiguous numeric version")
        endif()
        string(REGEX REPLACE "(..)" "0x\\1," RYZ_SIM_FACTORY_BYTES "${RYZ_SIM_FACTORY_HEX}")
        configure_file(../simulator/factory_source.h.in
            "${CMAKE_CURRENT_BINARY_DIR}/${RYZ_SIM_FACTORY_TAG}_source.h" @ONLY)
    endforeach()
    add_executable(v5_simulator MACOSX_BUNDLE ../simulator/main.c ../simulator/factory_demo.c
        "${RYZ_FIRMWARE}/components/ryz_workbench/workbench_telemetry.c"
        "${RYZ_FIRMWARE}/components/ryz_runtime/app_runtime.c"
        "${RYZ_FIRMWARE}/components/ryz_runtime/app_tools.c"
        "${RYZ_FIRMWARE}/managed_components/georgik__lua/lua/onelua.c")
    set_target_properties(v5_simulator PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "RyzoBee LVGL Simulator"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.ryzobee.lvgl-simulator")
    target_compile_definitions(v5_simulator PRIVATE MAKE_LIB)
    target_compile_options(v5_simulator PRIVATE -Wall -Wextra -Werror -UNDEBUG
        -include "${RYZ_FIRMWARE}/host/sdkconfig.h")
    target_include_directories(v5_simulator PRIVATE ${RYZ_LUA_PIXEL_INCLUDES}
        "${RYZ_FIRMWARE}/components/ryz_monitor" "${CMAKE_CURRENT_BINARY_DIR}"
        "${RYZ_FIRMWARE}/components/ryz_workbench"
        "${RYZ_FIRMWARE}/components/ryz_system_services/include"
        "${RYZ_FIRMWARE}/components/ryz_provisioning/include"
        "${RYZ_FIRMWARE}/components/ryz_time/include")
    target_link_libraries(v5_simulator PRIVATE ryz_v5_native ryz_v5_qr SDL2::SDL2 m)
    enable_testing()
    add_test(NAME v5_sdl_simulator COMMAND v5_simulator --self-test "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(v5_sdl_simulator PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software"
        TIMEOUT 30)
    add_test(NAME factory_sdl_simulator COMMAND v5_simulator --factory-self-test "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(factory_sdl_simulator PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software"
        TIMEOUT 30)
endif()
add_executable(v5_static_font_test "${RYZ_FIRMWARE}/tests/v5_static_font_test.c")
target_compile_options(v5_static_font_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(v5_static_font_test PRIVATE "${RYZ_FIRMWARE}/components/ryz_system_ui")
target_link_libraries(v5_static_font_test PRIVATE ryz_static_font ryz_font_offline)
add_executable(v5_ble_ui_test "${RYZ_FIRMWARE}/tests/v5_ble_ui_test.c")
add_executable(v5_display_ui_test "${RYZ_FIRMWARE}/tests/v5_display_ui_test.c")
target_compile_options(v5_display_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_display_ui_test PRIVATE ryz_v5_native ryz_v5_qr)
add_test(NAME v5_display_ui COMMAND v5_display_ui_test "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(v5_ble_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_ble_ui_test PRIVATE ryz_v5_native)
add_executable(v5_scripts_ui_test "${RYZ_FIRMWARE}/tests/v5_scripts_ui_test.c")
target_compile_options(v5_scripts_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_scripts_ui_test PRIVATE ryz_v5_native ryz_v5_qr)
add_executable(v5_shell_ui_test "${RYZ_FIRMWARE}/tests/v5_shell_ui_test.c")
target_compile_options(v5_shell_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_shell_ui_test PRIVATE ryz_v5_native ryz_v5_qr)
add_executable(v5_version_ui_test "${RYZ_FIRMWARE}/tests/v5_version_ui_test.c")
target_compile_options(v5_version_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_version_ui_test PRIVATE ryz_v5_native)
add_executable(v5_apps_ui_test "${RYZ_FIRMWARE}/tests/v5_apps_ui_test.c")
foreach(part IN ITEMS shell display apps lua)
    add_executable(ui_redraw_${part}_test "${RYZ_FIRMWARE}/tests/ui_redraw_${part}_test.c")
    target_compile_options(ui_redraw_${part}_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(ui_redraw_${part}_test PRIVATE ryz_v5_native ryz_v5_qr)
    add_test(NAME ui_redraw_${part} COMMAND ui_redraw_${part}_test)
endforeach()
target_compile_options(v5_apps_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_apps_ui_test PRIVATE ryz_v5_native ryz_v5_qr)
add_executable(v5_network_ui_test "${RYZ_FIRMWARE}/tests/v5_network_ui_test.c"
    "${RYZ_FIRMWARE}/managed_components/espressif__qrcode/qrcodegen.c")
target_include_directories(v5_network_ui_test PRIVATE
    "${RYZ_FIRMWARE}/managed_components/espressif__qrcode")
target_compile_options(v5_network_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_network_ui_test PRIVATE ryz_v5_native)
add_executable(v5_telemetry_test
    "${RYZ_FIRMWARE}/tests/v5_telemetry_test.c"
    "${RYZ_FIRMWARE}/components/ryz_workbench/workbench_telemetry.c")
target_compile_options(v5_telemetry_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(v5_telemetry_test PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_workbench"
    "${RYZ_FIRMWARE}/components/ryz_system_services/include"
    "${RYZ_FIRMWARE}/components/ryz_provisioning/include"
    "${RYZ_FIRMWARE}/components/ryz_time/include")
target_link_libraries(v5_telemetry_test PRIVATE ryz_v5_native ryz_v5_qr)
add_executable(v5_network_setup_test
    "${RYZ_FIRMWARE}/tests/v5_network_setup_test.c"
    "${RYZ_FIRMWARE}/components/ryz_workbench/workbench_network.c")
target_compile_options(v5_network_setup_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(v5_network_setup_test PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_workbench"
    "${RYZ_FIRMWARE}/components/ryz_system_services/include"
    "${RYZ_FIRMWARE}/components/ryz_provisioning/include"
    "${RYZ_FIRMWARE}/components/ryz_time/include")
target_link_libraries(v5_network_setup_test PRIVATE ryz_v5_native ryz_v5_qr)
add_executable(v5_password_ui_test "${RYZ_FIRMWARE}/tests/v5_password_ui_test.c")
target_compile_options(v5_password_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_password_ui_test PRIVATE ryz_v5_native ryz_v5_qr)
enable_testing()
add_test(NAME pixel_host_config COMMAND "${Python3_EXECUTABLE}"
    "${RYZ_FIRMWARE}/tools/check_pixel_host_config.py"
    "${RYZ_IDF_BUILD}" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME brand_font COMMAND brand_font_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME lvgl_lifecycle COMMAND lvgl_lifecycle_test)
add_test(NAME lvgl_fps COMMAND lvgl_fps_test)
add_test(NAME ui_update_pixels COMMAND ui_update_pixels
    "${RYZ_FIRMWARE}/scripts/ui_update_demo.lua" "${CMAKE_CURRENT_BINARY_DIR}")
foreach(case IN ITEMS happy errors unavailable gap cancel read-errors stress overload scroll)
    add_test(NAME monitor_script_${case} COMMAND monitor_script_pixels
        "${RYZ_FIRMWARE}/fs/tool_monitor.lua" ${case} "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
foreach(case IN ITEMS happy pins errors close-errors empty cancel scroll picker repeat)
    add_test(NAME i2c_script_${case} COMMAND i2c_script_pixels
        "${RYZ_FIRMWARE}/fs/tool_i2c.lua" ${case} "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
add_test(NAME v5_pixels COMMAND v5_pixels "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_static_font COMMAND v5_static_font_test)
add_executable(v5_boot_ui_test "${RYZ_FIRMWARE}/tests/v5_boot_ui_test.c")
target_compile_options(v5_boot_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(v5_boot_ui_test PRIVATE ryz_v5_native ryz_v5_qr)
add_test(NAME v5_boot_ui COMMAND v5_boot_ui_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_apps_ui COMMAND v5_apps_ui_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_network_ui COMMAND v5_network_ui_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_ble_ui COMMAND v5_ble_ui_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_scripts_ui COMMAND v5_scripts_ui_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_shell_ui COMMAND v5_shell_ui_test)
add_test(NAME v5_version_ui COMMAND v5_version_ui_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_telemetry COMMAND v5_telemetry_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_network_setup COMMAND v5_network_setup_test "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME v5_password_ui COMMAND v5_password_ui_test "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(workbench_version_test
    "${RYZ_FIRMWARE}/tests/workbench_version_test.c"
    "${RYZ_FIRMWARE}/components/ryz_workbench/workbench_version.c")
target_compile_options(workbench_version_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(workbench_version_test PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_workbench"
    "${RYZ_FIRMWARE}/components/ryz_ota/include")
target_link_libraries(workbench_version_test PRIVATE ryz_v5_native ryz_v5_qr)
add_test(NAME workbench_version COMMAND workbench_version_test "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(workbench_boot_key_test
    "${RYZ_FIRMWARE}/tests/workbench_boot_key_test.c"
    "${RYZ_FIRMWARE}/components/ryz_workbench/workbench_boot_key.c")
target_compile_options(workbench_boot_key_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(workbench_boot_key_test PRIVATE
    "${RYZ_FIRMWARE}/components/ryz_workbench/include")
add_test(NAME workbench_boot_key COMMAND workbench_boot_key_test)
