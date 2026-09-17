idf_component_register(
    SRCS "ryz_sensors.c" "ryz_sensors_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES ryz_board
    PRIV_REQUIRES esp_timer freertos)
