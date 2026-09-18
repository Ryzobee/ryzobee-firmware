idf_component_register(
    SRCS "workbench.c" "workbench_lifecycle.c" "workbench_io.c" "workbench_input.c"
         "workbench_file_rpc.c" "workbench_job_start.c" "workbench_i2c_rpc.c" "workbench_rgb_rpc.c"
         "workbench_monitor_rpc.c" "workbench_tools.c" "workbench_network_rpc.c" "workbench_apps.c"
         "workbench_telemetry.c" "workbench_scripts.c" "workbench_network.c" "workbench_restart.c"
         "workbench_version.c" "workbench_boot.c" "workbench_boot_key.c" "workbench_boot_events.c"
         "workbench_fault.c" "workbench_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES ryz_board ryz_runtime console mbedtls json
             esp_driver_uart esp_timer esp_psram spi_flash esp_app_format
             esp_system spiffs georgik__lua
    PRIV_REQUIRES ryz_ota ryz_provisioning ryz_ble ryz_system_ui ryz_time ryz_system_services ryz_lvgl
                  ryz_script_store ryz_script_metadata ryz_apps ryz_sensors ryz_i2c_scan ryz_rgb ryz_monitor ryz_tools ryz_diagnostics ryz_display_settings)
