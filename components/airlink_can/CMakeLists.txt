idf_component_register(SRCS "airlink_can.c" INCLUDE_DIRS "include"
    REQUIRES airlink_board airlink_led esp_driver_twai esp_timer esp_system freertos)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(airlink_libcanard_source
    GIT_REPOSITORY https://github.com/DroneCAN/libcanard.git
    GIT_TAG 601ed35467e0ac38819df17cd7c918de19f62d58
    GIT_SHALLOW FALSE)
FetchContent_GetProperties(airlink_libcanard_source)
if(NOT airlink_libcanard_source_POPULATED)
    FetchContent_Populate(airlink_libcanard_source)
endif()
add_library(airlink_libcanard STATIC "${airlink_libcanard_source_SOURCE_DIR}/canard.c")
target_include_directories(airlink_libcanard PUBLIC "${airlink_libcanard_source_SOURCE_DIR}")
target_compile_definitions(airlink_libcanard PUBLIC CANARD_ENABLE_CANFD=0 CANARD_MULTI_IFACE=0)
target_link_libraries(${COMPONENT_LIB} PUBLIC airlink_libcanard)
