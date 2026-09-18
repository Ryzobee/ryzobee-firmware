cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.10.2")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ryzobee_rootmaker)
set(ryz_factory_directory "${CMAKE_BINARY_DIR}/factory-scripts")
add_custom_target(ryz_factory_scripts
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_factory_scripts.py"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/fs" --output "${ryz_factory_directory}"
    VERBATIM)
spiffs_create_partition_image(scripts "${ryz_factory_directory}"
    FLASH_IN_PROJECT DEPENDS ryz_factory_scripts)
