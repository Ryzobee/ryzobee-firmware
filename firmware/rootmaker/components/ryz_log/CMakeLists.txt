idf_component_register(
    SRCS "ryz_log.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_timer heap log)
