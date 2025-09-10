cmake_minimum_required(VERSION 3.16)

# Prevent long object path issues on Windows
set(CMAKE_OBJECT_PATH_MAX 512)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Optional additional component directory (may have been removed after project reordering)
set(_extra_dirs)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../common_components")
    list(APPEND _extra_dirs ../common_components)
    message(STATUS "Using extra components from ../common_components")
else()
    message(STATUS "Skipping ../common_components (directory not found)")
endif()

# Include managed_components as a fallback search path so that locally vendored
# dependencies (e.g., lvgl__lvgl) are discoverable even if the component manager
# environment variable was disabled earlier.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
    list(APPEND _extra_dirs managed_components)
    message(STATUS "Including managed_components directory for local component resolution")
endif()

if(_extra_dirs)
    list(REMOVE_DUPLICATES _extra_dirs)
    set(EXTRA_COMPONENT_DIRS ${_extra_dirs})
endif()

# Project metadata
set(PROJECT_VER "1.0.1")
project(opdi_skpr VERSION ${PROJECT_VER})
add_compile_definitions(PROJECT_VER=\"${PROJECT_VER}\")

set(SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults)

# Suppress selected warnings (toolchain / 3rdparty noise)
add_compile_options(
    -Wno-format
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -Wno-unused-but-set-variable
    -Wno-cast-function-type
    -Wno-ignored-qualifiers
    -Wno-deprecated-enum-enum-conversion
    -Wno-write-strings
)
