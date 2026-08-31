cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)
if(NOT PROJECT_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-dev$")
    message(FATAL_ERROR "version.txt must contain one development semantic version")
endif()
project(airlink)

if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "6.0.2")
    message(FATAL_ERROR "AirLink C5 V1 qualification requires ESP-IDF 6.0.2 exactly")
endif()
