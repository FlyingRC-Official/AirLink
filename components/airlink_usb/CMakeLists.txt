idf_component_register(SRCS "airlink_usb.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core airlink_router airlink_config airlink_uart airlink_wifi airlink_can
             airlink_diag airlink_ota esp_app_format esp_driver_usb_serial_jtag esp_system soc
             freertos log)
