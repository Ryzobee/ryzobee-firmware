if(CONFIG_RYZ_LUA_FREETYPE)
  idf_component_register(
    SRCS "ryz_font.c" "ryz_font_bitmap.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/noto_sans_regular_ascii.ttf" "assets/noto_sans_semibold_ascii.ttf"
                "assets/teko_semibold_ascii.ttf" "assets/noto_sans_medium_ascii.ttf"
                "assets/roboto_mono_regular_ascii.ttf" "assets/roboto_mono_medium_ascii.ttf"
                "assets/roboto_mono_semibold_ascii.ttf"
    REQUIRES esp_common
    PRIV_REQUIRES espressif__freetype freertos log heap)
else()
  idf_component_register(SRCS "ryz_font_disabled.c"
      INCLUDE_DIRS "include" REQUIRES esp_common)
endif()
