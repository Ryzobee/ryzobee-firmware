idf_component_register(
    SRCS "ryz_apps.c" "ryz_apps_reader.c" "ryz_apps_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES ryz_script_store ryz_script_metadata)
