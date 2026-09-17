idf_component_register(SRCS "ryz_display_settings.c" "ryz_display_settings_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES ryz_board
    PRIV_REQUIRES nvs_flash freertos)
