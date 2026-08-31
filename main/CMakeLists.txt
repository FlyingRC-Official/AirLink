idf_component_register(SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES airlink_board airlink_config airlink_router airlink_uart
             airlink_wifi airlink_mesh airlink_usb airlink_can airlink_led airlink_diag
             airlink_ota airlink_web airlink_factory esp_system)
