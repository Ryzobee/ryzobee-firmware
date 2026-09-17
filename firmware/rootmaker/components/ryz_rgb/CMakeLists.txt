idf_component_register(
    SRCS "ryz_rgb.c" "ryz_rgb_esp.c" "ryz_rgb_driver.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_rmt esp_timer freertos)
