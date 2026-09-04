cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Firmware version. The release workflow passes the git tag (yyyy.mmdd.count)
# in ULANI_FW_VERSION; a plain local build has none and is a "dev" build. This
# becomes esp_app_get_description()->version, which the web UI reads back. A
# version that is not "dev" marks a production build (issue #37).
if(DEFINED ENV{ULANI_FW_VERSION} AND NOT "$ENV{ULANI_FW_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{ULANI_FW_VERSION}")
else()
    set(PROJECT_VER "dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ulani_esp32)
