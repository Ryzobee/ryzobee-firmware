idf_component_get_property(ryz_i2c_idf_dir esp_driver_i2c COMPONENT_DIR)
idf_component_register(
    SRCS "ryz_i2c_scan.c" "ryz_i2c_scan_esp.c" "ryz_i2c_scan_bus.c" "ryz_i2c_scan_idf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${ryz_i2c_idf_dir}"
    REQUIRES ryz_board ryz_tool_pins
    PRIV_REQUIRES esp_timer freertos esp_driver_i2c esp_driver_gpio esp_hw_support)
