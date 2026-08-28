idf_component_register(
    SRCS "airlink_can.c"
         "airlink_dronecan.c"
         "dsdl_generated/src/uavcan.protocol.NodeStatus.c"
         "dsdl_generated/src/uavcan.protocol.GetNodeInfo_res.c"
         "dsdl_generated/src/uavcan.tunnel.Targetted.c"
    INCLUDE_DIRS "include" "dsdl_generated/include"
    REQUIRES airlink_board airlink_led airlink_router esp_app_format esp_driver_twai esp_timer esp_system freertos)

# Vendored from DroneCAN/libcanard commit
# 601ed35467e0ac38819df17cd7c918de19f62d58. Keeping the three-file library
# local makes tagged firmware builds reproducible without a network fetch.
add_library(airlink_libcanard STATIC "${CMAKE_CURRENT_LIST_DIR}/libcanard/canard.c")
target_include_directories(airlink_libcanard PUBLIC "${CMAKE_CURRENT_LIST_DIR}/libcanard")
target_compile_definitions(airlink_libcanard PUBLIC CANARD_ENABLE_CANFD=0 CANARD_MULTI_IFACE=0)
target_link_libraries(${COMPONENT_LIB} PUBLIC airlink_libcanard)
