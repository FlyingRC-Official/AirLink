idf_component_register(SRCS "airlink_uart.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core airlink_board airlink_router esp_driver_uart esp_timer esp_system freertos)
