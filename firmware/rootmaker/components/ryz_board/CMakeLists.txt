idf_component_get_property(ryz_board_i2c_idf_dir esp_driver_i2c COMPONENT_DIR)
idf_component_register(
    SRCS "display.c" "board_i2c.c" "i2c_idf_compat.c" "imu.c" "touch.c" "touch_protocol.c" "touch_demo.c" "boot_button.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${ryz_board_i2c_idf_dir}"
    REQUIRES esp_lcd esp_timer
             esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_i2c
    PRIV_REQUIRES log freertos esp_pm esp_ringbuf)

# The compatibility source pins the audited private ABI to ESP-IDF 5.5.4.
# Apply to final link so SDK acquire references wrap. Force extraction of this
# archive member even when the SDK's wrapped references appear later in the link.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=i2c_acquire_bus_handle"
    "-Wl,--undefined=__wrap_i2c_acquire_bus_handle")
