idf_component_register(SRCS "airlink_usb.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core airlink_router airlink_config esp_driver_usb_serial_jtag esp_system freertos log)
