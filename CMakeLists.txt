cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airlink VERSION 0.3.3)

if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "6.0.2")
    message(FATAL_ERROR "AirLink C5 V1 qualification requires ESP-IDF 6.0.2 exactly")
endif()
