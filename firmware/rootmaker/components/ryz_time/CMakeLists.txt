idf_component_register(
    SRCS "ryz_time.c" "ryz_time_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES esp_netif esp_timer freertos lwip)
