idf_component_register(SRCS "airlink_led.c" INCLUDE_DIRS "include"
    REQUIRES airlink_board airlink_router espressif__led_strip freertos)
