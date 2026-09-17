idf_component_register(
    SRCS "ryz_system_ui.c" "ryz_qr_encoder_esp.c" "ryz_ui_navigation.c"
         "ryz_v5_render.c" "ryz_v5_widgets.c" "ryz_v5_assets.c" "ryz_v5_apps.c"
         "ryz_v5_boot.c" "ryz_v5_boot_asset.c"
         "ryz_v5_fault.c"
         "ryz_v5_display.c"
         "ryz_v5_network.c" "ryz_v5_password.c" "ryz_v5_version.c"
         "ryz_v5_ble.c" "ryz_v5_scripts.c" "ryz_v5_status.c" "ryz_v5_render_dirty.c"
    INCLUDE_DIRS "include"
    REQUIRES ryz_board ryz_lvgl ryz_apps ryz_display_settings ryz_provisioning
    PRIV_REQUIRES espressif__qrcode log)

# qrcode 0.2.0 logs the full input text at INFO. Compile that dependency with
# local logging disabled because its input contains the SoftAP password.
idf_component_get_property(qrcode_lib espressif__qrcode COMPONENT_LIB)
target_compile_definitions(${qrcode_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_NONE)
