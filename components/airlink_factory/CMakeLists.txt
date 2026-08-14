idf_component_register(SRCS "airlink_factory.c" "airlink_factory_ble.c" INCLUDE_DIRS "include"
    REQUIRES airlink_board airlink_config airlink_uart airlink_can airlink_wifi
             airlink_usb airlink_led esp_wifi esp_system bt)
